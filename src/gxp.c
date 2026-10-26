/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/gxp.h"

#include <stdint.h>

#define GXP_MAGIC UINT32_C(0x00505847)
#define GXP_HEADER_SIZE 76u
#define GXP_FLAGS_OFFSET 20u
#define GXP_PRIMARY_COUNT_OFFSET 60u
#define GXP_PRIMARY_RELATIVE_OFFSET 64u
#define GXP_SECONDARY_COUNT_OFFSET 68u
#define GXP_SECONDARY_RELATIVE_OFFSET 72u
#define GXP_INSTRUCTION_SIZE 8u

static uint32_t read_u32_le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
}

static vita_gxp_parse_result parse_stream(
    const uint8_t *bytes, uint32_t declared_size, uint32_t count_offset,
    uint32_t relative_offset, vita_gxp_instruction_stream *stream) {
    const uint32_t instruction_count = read_u32_le(bytes + count_offset);
    const uint32_t encoded_relative = read_u32_le(bytes + relative_offset);
    const uint64_t stream_offset =
        (uint64_t)relative_offset + encoded_relative;

    if (instruction_count > UINT32_MAX / GXP_INSTRUCTION_SIZE)
        return VITA_GXP_PARSE_INSTRUCTION_COUNT_OVERFLOW;
    const uint32_t stream_size =
        instruction_count * GXP_INSTRUCTION_SIZE;
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
    result = parse_stream(
        bytes, declared_size, GXP_PRIMARY_COUNT_OFFSET,
        GXP_PRIMARY_RELATIVE_OFFSET, &parsed.primary);
    if (result != VITA_GXP_PARSE_OK)
        return result;
    result = parse_stream(
        bytes, declared_size, GXP_SECONDARY_COUNT_OFFSET,
        GXP_SECONDARY_RELATIVE_OFFSET, &parsed.secondary);
    if (result != VITA_GXP_PARSE_OK)
        return result;

    *program = parsed;
    return VITA_GXP_PARSE_OK;
}
