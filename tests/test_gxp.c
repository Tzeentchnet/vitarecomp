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

    assert(vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX,
                          &program) == VITA_GXP_PARSE_OK);
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
    assert(vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX,
                          &program) == VITA_GXP_PARSE_BAD_MAGIC);
    write_u32_le(bytes, 0u, UINT32_C(0x00505847));
    assert(vita_gxp_parse(bytes, 75u, VITA_GXP_STAGE_VERTEX, &program) ==
           VITA_GXP_PARSE_TRUNCATED);
    write_u32_le(bytes, 8u, 75u);
    assert(vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX,
                          &program) == VITA_GXP_PARSE_BAD_DECLARED_SIZE);
    write_u32_le(bytes, 8u, 129u);
    assert(vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX,
                          &program) == VITA_GXP_PARSE_BAD_DECLARED_SIZE);
}

static void test_count_and_relative_bounds(void) {
    uint8_t bytes[128];
    vita_gxp_program_view program;
    make_valid_vertex_program(bytes, sizeof(bytes));

    write_u32_le(bytes, 60u, UINT32_C(0x20000000));
    assert(
        vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
        VITA_GXP_PARSE_INSTRUCTION_COUNT_OVERFLOW);

    make_valid_vertex_program(bytes, sizeof(bytes));
    write_u32_le(bytes, 64u, UINT32_C(0x7FFFFFFF));
    assert(
        vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
        VITA_GXP_PARSE_INSTRUCTION_SPAN_OUT_OF_RANGE);

    make_valid_vertex_program(bytes, sizeof(bytes));
    write_u32_le(bytes, 64u, UINT32_C(0x80000000));
    assert(
        vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
        VITA_GXP_PARSE_INSTRUCTION_SPAN_OUT_OF_RANGE);

    make_valid_vertex_program(bytes, sizeof(bytes));
    write_u32_le(bytes, 64u, 56u);
    assert(
        vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX, &program) ==
        VITA_GXP_PARSE_INSTRUCTION_SPAN_OUT_OF_RANGE);
}

static void test_stage_requirement(void) {
    uint8_t bytes[128];
    vita_gxp_program_view program;
    make_valid_vertex_program(bytes, sizeof(bytes));
    write_u32_le(bytes, 20u, 1u);

    assert(vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX,
                          &program) == VITA_GXP_PARSE_WRONG_STAGE);
    assert(vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_FRAGMENT,
                          &program) == VITA_GXP_PARSE_OK);
    assert(program.stage == VITA_GXP_STAGE_FRAGMENT);
    assert(vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_ANY, &program) ==
           VITA_GXP_PARSE_OK);
}

static vita_gxp_translation_request
make_translation_request(const vita_gxp_program_view *program,
                         const uint8_t *attributes, size_t attribute_size,
                         const uint8_t *streams, size_t stream_size) {
    const vita_gxp_translation_request request = {
        program,
        VITA_GXP_TRANSLATOR_ABI_VERSION,
        1u,
        VITA_GXP_REGISTER_INITIALIZATION_ZERO,
        0u,
        attributes,
        attribute_size,
        streams,
        stream_size,
    };
    return request;
}

static void assert_key_changed(const vita_gxp_translation_key *left,
                               const vita_gxp_translation_key *right) {
    assert(memcmp(left->bytes, right->bytes, sizeof(left->bytes)) != 0);
}

static void test_translation_key_identity(void) {
    uint8_t bytes[128];
    uint8_t attributes[16] = {
        0u, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 1u, 0u, 7u, 8u, 9u, 10u, 11u, 12u,
    };
    uint8_t streams[8] = {4u, 0u, 0u, 0u, 8u, 0u, 0u, 0u};
    vita_gxp_program_view program;
    vita_gxp_translation_request request;
    vita_gxp_translation_key baseline;
    vita_gxp_translation_key repeated;
    vita_gxp_translation_key changed;

    make_valid_vertex_program(bytes, sizeof(bytes));
    assert(vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX,
                          &program) == VITA_GXP_PARSE_OK);
    request = make_translation_request(&program, attributes, sizeof(attributes),
                                       streams, sizeof(streams));
    assert(vita_gxp_make_translation_key(&request, &baseline) ==
           VITA_GXP_TRANSLATION_KEY_OK);
    assert(vita_gxp_make_translation_key(&request, &repeated) ==
           VITA_GXP_TRANSLATION_KEY_OK);
    assert(memcmp(baseline.bytes, repeated.bytes, sizeof(baseline.bytes)) == 0);

    bytes[40] = 1u;
    assert(vita_gxp_make_translation_key(&request, &changed) ==
           VITA_GXP_TRANSLATION_KEY_OK);
    assert_key_changed(&baseline, &changed);
    bytes[40] = 0u;

    request.translator_abi_version++;
    assert(vita_gxp_make_translation_key(&request, &changed) ==
           VITA_GXP_TRANSLATION_KEY_OK);
    assert_key_changed(&baseline, &changed);
    request.translator_abi_version--;

    request.hlsl_abi_version++;
    assert(vita_gxp_make_translation_key(&request, &changed) ==
           VITA_GXP_TRANSLATION_KEY_OK);
    assert_key_changed(&baseline, &changed);
    request.hlsl_abi_version--;

    request.feature_mask = UINT64_C(0x8000000000000001);
    assert(vita_gxp_make_translation_key(&request, &changed) ==
           VITA_GXP_TRANSLATION_KEY_OK);
    assert_key_changed(&baseline, &changed);
    request.feature_mask = 0u;

    attributes[5] ^= UINT8_C(0x80);
    assert(vita_gxp_make_translation_key(&request, &changed) ==
           VITA_GXP_TRANSLATION_KEY_OK);
    assert_key_changed(&baseline, &changed);
    attributes[5] ^= UINT8_C(0x80);

    streams[4] ^= UINT8_C(0x10);
    assert(vita_gxp_make_translation_key(&request, &changed) ==
           VITA_GXP_TRANSLATION_KEY_OK);
    assert_key_changed(&baseline, &changed);
}

static void test_translation_key_validation_preserves_output(void) {
    uint8_t bytes[128];
    uint8_t attributes[8] = {0};
    vita_gxp_program_view program;
    vita_gxp_translation_request request;
    vita_gxp_translation_key key;
    vita_gxp_translation_key original;

    make_valid_vertex_program(bytes, sizeof(bytes));
    assert(vita_gxp_parse(bytes, sizeof(bytes), VITA_GXP_STAGE_VERTEX,
                          &program) == VITA_GXP_PARSE_OK);
    request = make_translation_request(&program, attributes, sizeof(attributes),
                                       NULL, 0u);
    memset(&key, 0xA5, sizeof(key));
    original = key;

    request.vertex_attribute_bytes = NULL;
    assert(vita_gxp_make_translation_key(&request, &key) ==
           VITA_GXP_TRANSLATION_KEY_INVALID_LAYOUT);
    assert(memcmp(&key, &original, sizeof(key)) == 0);
    request.vertex_attribute_bytes = attributes;

    request.register_initialization = (vita_gxp_register_initialization)0u;
    assert(vita_gxp_make_translation_key(&request, &key) ==
           VITA_GXP_TRANSLATION_KEY_INVALID_ARGUMENT);
    assert(memcmp(&key, &original, sizeof(key)) == 0);
    request.register_initialization = VITA_GXP_REGISTER_INITIALIZATION_ZERO;

    program.primary.bytes = bytes + sizeof(bytes) - 4u;
    assert(vita_gxp_make_translation_key(&request, &key) ==
           VITA_GXP_TRANSLATION_KEY_INVALID_PROGRAM_VIEW);
    assert(memcmp(&key, &original, sizeof(key)) == 0);

    assert(strcmp(vita_gxp_translation_key_result_name(
                      VITA_GXP_TRANSLATION_KEY_INVALID_LAYOUT),
                  "INVALID_LAYOUT") == 0);
    assert(strcmp(vita_gxp_translation_key_result_name(
                      (vita_gxp_translation_key_result)UINT32_MAX),
                  "INVALID") == 0);
}

int main(void) {
    test_valid_program();
    test_truncated_and_declared_sizes();
    test_count_and_relative_bounds();
    test_stage_requirement();
    test_translation_key_identity();
    test_translation_key_validation_preserves_output();
    puts("bounded GXP parser and translation-key tests passed");
    return 0;
}
