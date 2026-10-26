/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/gxp.h"
#include "sha256.h"

#include <stdint.h>
#include <string.h>

#define GXP_MAGIC UINT32_C(0x00505847)
#define GXP_HEADER_SIZE 76u
#define GXP_FLAGS_OFFSET 20u
#define GXP_PRIMARY_COUNT_OFFSET 60u
#define GXP_PRIMARY_RELATIVE_OFFSET 64u
#define GXP_SECONDARY_COUNT_OFFSET 68u
#define GXP_SECONDARY_RELATIVE_OFFSET 72u
#define GXP_INSTRUCTION_SIZE 8u

static uint32_t read_u32_le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static vita_gxp_parse_result parse_stream(const uint8_t *bytes,
                                          uint32_t declared_size,
                                          uint32_t count_offset,
                                          uint32_t relative_offset,
                                          vita_gxp_instruction_stream *stream) {
    const uint32_t instruction_count = read_u32_le(bytes + count_offset);
    const uint32_t encoded_relative = read_u32_le(bytes + relative_offset);
    const uint64_t stream_offset = (uint64_t)relative_offset + encoded_relative;

    if (instruction_count > UINT32_MAX / GXP_INSTRUCTION_SIZE)
        return VITA_GXP_PARSE_INSTRUCTION_COUNT_OVERFLOW;
    const uint32_t stream_size = instruction_count * GXP_INSTRUCTION_SIZE;
    if (stream_offset > declared_size ||
        stream_size > declared_size - (uint32_t)stream_offset)
        return VITA_GXP_PARSE_INSTRUCTION_SPAN_OUT_OF_RANGE;

    stream->bytes = bytes + (size_t)stream_offset;
    stream->byte_count = stream_size;
    stream->instruction_count = instruction_count;
    return VITA_GXP_PARSE_OK;
}

vita_gxp_parse_result vita_gxp_parse(const uint8_t *bytes, size_t byte_count,
                                     vita_gxp_stage required_stage,
                                     vita_gxp_program_view *program) {
    vita_gxp_program_view parsed;
    vita_gxp_parse_result result;

    if (!bytes || !program ||
        (required_stage != VITA_GXP_STAGE_VERTEX &&
         required_stage != VITA_GXP_STAGE_FRAGMENT &&
         required_stage != VITA_GXP_STAGE_ANY))
        return VITA_GXP_PARSE_INVALID_ARGUMENT;
    if (byte_count < GXP_HEADER_SIZE)
        return VITA_GXP_PARSE_TRUNCATED;
    if (read_u32_le(bytes) != GXP_MAGIC)
        return VITA_GXP_PARSE_BAD_MAGIC;

    const uint32_t declared_size = read_u32_le(bytes + 8u);
    if (declared_size < GXP_HEADER_SIZE || declared_size > byte_count)
        return VITA_GXP_PARSE_BAD_DECLARED_SIZE;

    const uint32_t flags = read_u32_le(bytes + GXP_FLAGS_OFFSET);
    const vita_gxp_stage stage =
        (flags & 1u) ? VITA_GXP_STAGE_FRAGMENT : VITA_GXP_STAGE_VERTEX;
    if (required_stage != VITA_GXP_STAGE_ANY && stage != required_stage)
        return VITA_GXP_PARSE_WRONG_STAGE;

    parsed.bytes = bytes;
    parsed.declared_size = declared_size;
    parsed.flags = flags;
    parsed.stage = stage;
    result = parse_stream(bytes, declared_size, GXP_PRIMARY_COUNT_OFFSET,
                          GXP_PRIMARY_RELATIVE_OFFSET, &parsed.primary);
    if (result != VITA_GXP_PARSE_OK)
        return result;
    result = parse_stream(bytes, declared_size, GXP_SECONDARY_COUNT_OFFSET,
                          GXP_SECONDARY_RELATIVE_OFFSET, &parsed.secondary);
    if (result != VITA_GXP_PARSE_OK)
        return result;

    *program = parsed;
    return VITA_GXP_PARSE_OK;
}

static int stream_is_within_program(const vita_gxp_program_view *program,
                                    const vita_gxp_instruction_stream *stream) {
    const uintptr_t base = (uintptr_t)program->bytes;
    const uintptr_t stream_address = (uintptr_t)stream->bytes;
    uintptr_t end;

    if (program->declared_size > UINTPTR_MAX - base)
        return 0;
    end = base + program->declared_size;
    if ((stream->instruction_count != 0u && !stream->bytes) ||
        stream->instruction_count > UINT32_MAX / GXP_INSTRUCTION_SIZE ||
        stream->byte_count !=
            (size_t)stream->instruction_count * GXP_INSTRUCTION_SIZE)
        return 0;
    if (stream->byte_count == 0u)
        return !stream->bytes ||
               (stream_address >= base && stream_address <= end);
    return stream_address >= base && stream_address < end &&
           stream->byte_count <= end - stream_address;
}

vita_gxp_translation_key_result
vita_gxp_make_translation_key(const vita_gxp_translation_request *request,
                              vita_gxp_translation_key *key) {
    static const char DOMAIN[] = "vitarecomp.gxp-translation.v1";
    vita_sha256_context hash;
    vita_gxp_translation_key generated;
    const vita_gxp_program_view *program;

    if (!request || !key || !request->program)
        return VITA_GXP_TRANSLATION_KEY_INVALID_ARGUMENT;
    program = request->program;
    if (!program->bytes || program->declared_size < GXP_HEADER_SIZE ||
        (program->stage != VITA_GXP_STAGE_VERTEX &&
         program->stage != VITA_GXP_STAGE_FRAGMENT) ||
        !stream_is_within_program(program, &program->primary) ||
        !stream_is_within_program(program, &program->secondary))
        return VITA_GXP_TRANSLATION_KEY_INVALID_PROGRAM_VIEW;
    if (request->translator_abi_version == 0u ||
        request->hlsl_abi_version == 0u ||
        request->register_initialization !=
            VITA_GXP_REGISTER_INITIALIZATION_ZERO)
        return VITA_GXP_TRANSLATION_KEY_INVALID_ARGUMENT;
    if ((request->vertex_attribute_bytes == NULL) !=
            (request->vertex_attribute_byte_count == 0u) ||
        (request->vertex_stream_bytes == NULL) !=
            (request->vertex_stream_byte_count == 0u) ||
        request->vertex_attribute_byte_count > UINT32_MAX ||
        request->vertex_stream_byte_count > UINT32_MAX ||
        (program->stage != VITA_GXP_STAGE_VERTEX &&
         (request->vertex_attribute_byte_count != 0u ||
          request->vertex_stream_byte_count != 0u)))
        return VITA_GXP_TRANSLATION_KEY_INVALID_LAYOUT;

    vita_sha256_init(&hash);
    vita_sha256_update(&hash, DOMAIN, sizeof(DOMAIN) - 1u);
    vita_sha256_update_u32(&hash, (uint32_t)program->stage);
    vita_sha256_update_u32(&hash, request->translator_abi_version);
    vita_sha256_update_u32(&hash, request->hlsl_abi_version);
    vita_sha256_update_u32(&hash, (uint32_t)request->register_initialization);
    vita_sha256_update_u64(&hash, request->feature_mask);
    vita_sha256_update_field(&hash, program->bytes, program->declared_size);
    vita_sha256_update_field(&hash, request->vertex_attribute_bytes,
                             request->vertex_attribute_byte_count);
    vita_sha256_update_field(&hash, request->vertex_stream_bytes,
                             request->vertex_stream_byte_count);
    vita_sha256_finish(&hash, generated.bytes);
    *key = generated;
    return VITA_GXP_TRANSLATION_KEY_OK;
}

const char *
vita_gxp_translation_key_result_name(vita_gxp_translation_key_result result) {
    switch (result) {
    case VITA_GXP_TRANSLATION_KEY_OK:
        return "OK";
    case VITA_GXP_TRANSLATION_KEY_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case VITA_GXP_TRANSLATION_KEY_INVALID_PROGRAM_VIEW:
        return "INVALID_PROGRAM_VIEW";
    case VITA_GXP_TRANSLATION_KEY_INVALID_LAYOUT:
        return "INVALID_LAYOUT";
    }
    return "INVALID";
}
