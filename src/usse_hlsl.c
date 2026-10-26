/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/usse.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hlsl_builder {
    char *data;
    size_t capacity;
    size_t length;
    int failed;
} hlsl_builder;

static void append_format(hlsl_builder *builder, const char *format, ...) {
    va_list args;
    va_list measure_args;
    int written;

    if (builder->failed)
        return;
    va_start(args, format);
    va_copy(measure_args, args);
    written = vsnprintf(NULL, 0u, format, measure_args);
    va_end(measure_args);
    if (written < 0) {
        builder->failed = 1;
        va_end(args);
        return;
    }
    if (builder->data) {
        if ((size_t)written >= builder->capacity - builder->length) {
            builder->failed = 1;
            va_end(args);
            return;
        }
        if (vsnprintf(builder->data + builder->length,
                      builder->capacity - builder->length, format,
                      args) != written) {
            builder->failed = 1;
            va_end(args);
            return;
        }
    }
    va_end(args);
    builder->length += (size_t)written;
}

static uint64_t read_u64_le(const uint8_t *bytes) {
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; index++)
        value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

static int validate_stream(const vita_usse_stream_view *stream) {
    const uint64_t required = (uint64_t)stream->instruction_count * UINT64_C(8);
    return (stream->instruction_count == 0u || stream->bytes) &&
           required <= SIZE_MAX && required <= stream->byte_count;
}

static const char *bank_array_name(vita_usse_register_bank bank) {
    switch (bank) {
    case VITA_USSE_REGISTER_TEMPORARY:
        return "vita_temporary";
    case VITA_USSE_REGISTER_PRIMARY:
        return "vita_primary";
    case VITA_USSE_REGISTER_OUTPUT:
        return "vita_output";
    case VITA_USSE_REGISTER_SECONDARY:
        return "vita_secondary";
    default:
        return NULL;
    }
}

static int swizzle_is_component(vita_usse_swizzle_channel channel) {
    return channel >= VITA_USSE_SWIZZLE_X && channel <= VITA_USSE_SWIZZLE_W;
}

static int operand_component_in_range(const vita_usse_operand *operand,
                                      uint32_t lane) {
    const vita_usse_swizzle_channel channel = operand->swizzle[lane];
    if (!swizzle_is_component(channel))
        return channel >= VITA_USSE_SWIZZLE_ZERO &&
               channel <= VITA_USSE_SWIZZLE_HALF;
    if (operand->bank == VITA_USSE_REGISTER_INTERNAL)
        return operand->index < VITA_USSE_INTERNAL_REGISTER_COUNT;
    return bank_array_name(operand->bank) &&
           operand->index + (uint32_t)channel < VITA_USSE_REGISTER_WORD_COUNT;
}

static void append_raw_operand_component(hlsl_builder *builder,
                                         const vita_usse_operand *operand,
                                         uint32_t lane) {
    static const char components[4] = {'x', 'y', 'z', 'w'};
    const vita_usse_swizzle_channel channel = operand->swizzle[lane];
    const char *array_name;

    switch (channel) {
    case VITA_USSE_SWIZZLE_ZERO:
        append_format(builder, "0.0f");
        return;
    case VITA_USSE_SWIZZLE_ONE:
        append_format(builder, "1.0f");
        return;
    case VITA_USSE_SWIZZLE_TWO:
        append_format(builder, "2.0f");
        return;
    case VITA_USSE_SWIZZLE_HALF:
        append_format(builder, "0.5f");
        return;
    default:
        break;
    }

    if (operand->bank == VITA_USSE_REGISTER_INTERNAL) {
        append_format(builder, "vita_internal[%u].%c", operand->index,
                      components[(uint32_t)channel]);
        return;
    }
    array_name = bank_array_name(operand->bank);
    append_format(builder, "%s[%u]", array_name,
                  operand->index + (uint32_t)channel);
}

static void append_operand_component(hlsl_builder *builder,
                                     const vita_usse_operand *operand,
                                     uint32_t lane) {
    const int absolute = (operand->flags & VITA_USSE_OPERAND_ABSOLUTE) != 0u;
    const int negate = (operand->flags & VITA_USSE_OPERAND_NEGATE) != 0u;

    if (negate)
        append_format(builder, "-(");
    if (absolute)
        append_format(builder, "abs(");
    append_raw_operand_component(builder, operand, lane);
    if (absolute)
        append_format(builder, ")");
    if (negate)
        append_format(builder, ")");
}

static int destination_in_range(const vita_usse_operand *destination,
                                uint32_t lane) {
    if (destination->bank == VITA_USSE_REGISTER_INTERNAL)
        return destination->index < VITA_USSE_INTERNAL_REGISTER_COUNT;
    return bank_array_name(destination->bank) &&
           destination->index + lane < VITA_USSE_REGISTER_WORD_COUNT;
}

static void append_destination(hlsl_builder *builder,
                               const vita_usse_operand *destination,
                               uint32_t lane) {
    static const char components[4] = {'x', 'y', 'z', 'w'};
    if (destination->bank == VITA_USSE_REGISTER_INTERNAL) {
        append_format(builder, "vita_internal[%u].%c", destination->index,
                      components[lane]);
    } else {
        append_format(builder, "%s[%u]", bank_array_name(destination->bank),
                      destination->index + lane);
    }
}

static int instruction_operands_in_range(
    const vita_usse_decoded_instruction *instruction) {
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        if ((instruction->write_mask & (1u << lane)) != 0u &&
            !destination_in_range(&instruction->destination, lane))
            return 0;
        for (uint32_t source = 0u; source < instruction->source_count;
             source++) {
            if (!operand_component_in_range(&instruction->sources[source],
                                            lane))
                return 0;
        }
    }
    return 1;
}

static void append_operand_vector(hlsl_builder *builder,
                                  const vita_usse_operand *operand) {
    append_format(builder, "float4(");
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        if (lane != 0u)
            append_format(builder, ", ");
        append_operand_component(builder, operand, lane);
    }
    append_format(builder, ")");
}

static int emit_pack(hlsl_builder *builder,
                     const vita_usse_decoded_instruction *instruction) {
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        vita_usse_operand source;
        uint32_t source_index;
        uint32_t source_lane;

        if ((instruction->write_mask & (1u << lane)) == 0u)
            continue;
        source_index = instruction->source_select[lane] < 2u ? 0u : 1u;
        source_lane = instruction->source_select[lane] & 1u;
        source = instruction->sources[source_index];
        source.swizzle[lane] = (vita_usse_swizzle_channel)source_lane;
        if (!operand_component_in_range(&source, lane))
            return 0;
        append_format(builder, "    ");
        append_destination(builder, &instruction->destination, lane);
        append_format(builder, " = ");
        append_operand_component(builder, &source, lane);
        append_format(builder, ";\n");
    }
    return 1;
}

static int emit_alu(hlsl_builder *builder,
                    const vita_usse_decoded_instruction *instruction) {
    if (!instruction_operands_in_range(instruction))
        return 0;

    if (instruction->operation == VITA_USSE_SEMANTIC_DOT) {
        uint32_t destination_lane = 0u;
        while ((instruction->write_mask & (UINT32_C(1) << destination_lane)) ==
               0u)
            destination_lane++;
        append_format(builder, "    ");
        append_destination(builder, &instruction->destination,
                           destination_lane);
        append_format(builder, " = dot(");
        append_operand_vector(builder, &instruction->sources[0]);
        append_format(builder, ", ");
        append_operand_vector(builder, &instruction->sources[1]);
        append_format(builder, ");\n");
        return 1;
    }

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        if ((instruction->write_mask & (1u << lane)) == 0u)
            continue;
        append_format(builder, "    ");
        append_destination(builder, &instruction->destination, lane);
        append_format(builder, " = ");
        switch (instruction->operation) {
        case VITA_USSE_SEMANTIC_FMA:
            append_format(builder, "mad(");
            append_operand_component(builder, &instruction->sources[0], lane);
            append_format(builder, ", ");
            append_operand_component(builder, &instruction->sources[1], lane);
            append_format(builder, ", ");
            append_operand_component(builder, &instruction->sources[2], lane);
            append_format(builder, ")");
            break;
        case VITA_USSE_SEMANTIC_MUL:
            append_operand_component(builder, &instruction->sources[0], lane);
            append_format(builder, " * ");
            append_operand_component(builder, &instruction->sources[1], lane);
            break;
        case VITA_USSE_SEMANTIC_ADD:
            append_operand_component(builder, &instruction->sources[0], lane);
            append_format(builder, " + ");
            append_operand_component(builder, &instruction->sources[1], lane);
            break;
        case VITA_USSE_SEMANTIC_FRACT:
            append_format(builder, "frac(");
            append_operand_component(builder, &instruction->sources[0], lane);
            append_format(builder, ")");
            break;
        case VITA_USSE_SEMANTIC_MIN:
        case VITA_USSE_SEMANTIC_MAX:
            append_format(builder,
                          instruction->operation == VITA_USSE_SEMANTIC_MIN
                              ? "min("
                              : "max(");
            append_operand_component(builder, &instruction->sources[0], lane);
            append_format(builder, ", ");
            append_operand_component(builder, &instruction->sources[1], lane);
            append_format(builder, ")");
            break;
        default:
            return 0;
        }
        append_format(builder, ";\n");
    }
    return !builder->failed;
}

static void emit_common_locals(hlsl_builder *builder) {
    append_format(builder, "    float vita_temporary[128];\n"
                           "    float vita_primary[128];\n"
                           "    float vita_output[128];\n"
                           "    float vita_secondary[128];\n"
                           "    float4 vita_internal[4];\n"
                           "    [unroll] for (uint vita_i = 0u; vita_i < 128u; "
                           "vita_i++) {\n"
                           "        vita_temporary[vita_i] = 0.0f;\n"
                           "        vita_primary[vita_i] = 0.0f;\n"
                           "        vita_output[vita_i] = 0.0f;\n"
                           "        vita_secondary[vita_i] = 0.0f;\n"
                           "    }\n"
                           "    [unroll] for (uint vita_i = 0u; vita_i < 4u; "
                           "vita_i++)\n"
                           "        vita_internal[vita_i] = 0.0f;\n");
}

static void emit_vertex_header(hlsl_builder *builder) {
    append_format(builder,
                  "// vitarecomp USSE HLSL ABI %u\n"
                  "struct VitaVertexInput {\n",
                  VITA_USSE_HLSL_ABI_VERSION);
    for (uint32_t index = 0u; index < 16u; index++)
        append_format(builder, "    float4 primary%u : TEXCOORD%u;\n", index,
                      index);
    append_format(builder, "};\n"
                           "struct VitaVertexOutput {\n"
                           "    float4 position : SV_Position;\n");
    for (uint32_t index = 0u; index < 8u; index++)
        append_format(builder, "    float4 varying%u : TEXCOORD%u;\n", index,
                      index);
    append_format(builder,
                  "};\n"
                  "cbuffer VitaSecondaryAttributes : register(b0) {\n"
                  "    float4 vita_secondary_input[32];\n"
                  "};\n"
                  "VitaVertexOutput vita_main(VitaVertexInput input) {\n");
    emit_common_locals(builder);
    for (uint32_t word = 0u; word < 64u; word++) {
        static const char components[4] = {'x', 'y', 'z', 'w'};
        append_format(builder, "    vita_primary[%u] = input.primary%u.%c;\n",
                      word, word / 4u, components[word % 4u]);
    }
    for (uint32_t word = 0u; word < 128u; word++) {
        static const char components[4] = {'x', 'y', 'z', 'w'};
        append_format(builder,
                      "    vita_secondary[%u] = "
                      "vita_secondary_input[%u].%c;\n",
                      word, word / 4u, components[word % 4u]);
    }
}

static void emit_compute_header(hlsl_builder *builder) {
    append_format(builder,
                  "// vitarecomp USSE HLSL ABI %u\n"
                  "RWStructuredBuffer<float> vita_state : register(u0);\n"
                  "[numthreads(1, 1, 1)]\n"
                  "void vita_main(uint3 thread_id : SV_DispatchThreadID) {\n"
                  "    if (thread_id.y != 0u || thread_id.z != 0u)\n"
                  "        return;\n",
                  VITA_USSE_HLSL_ABI_VERSION);
    emit_common_locals(builder);
    append_format(builder,
                  "    const uint vita_state_base = thread_id.x * %uu;\n"
                  "    [unroll] for (uint vita_i = 0u; vita_i < 128u; "
                  "vita_i++) {\n"
                  "        vita_primary[vita_i] = "
                  "vita_state[vita_state_base + %uu + vita_i];\n"
                  "        vita_secondary[vita_i] = "
                  "vita_state[vita_state_base + %uu + vita_i];\n"
                  "    }\n",
                  VITA_USSE_HLSL_STATE_WORD_COUNT,
                  VITA_USSE_HLSL_STATE_PRIMARY_OFFSET,
                  VITA_USSE_HLSL_STATE_SECONDARY_OFFSET);
}

static void emit_vertex_footer(hlsl_builder *builder) {
    append_format(builder,
                  "    VitaVertexOutput result = (VitaVertexOutput)0;\n"
                  "    result.position = float4(vita_output[0], "
                  "vita_output[1], vita_output[2], vita_output[3]);\n");
    for (uint32_t index = 0u; index < 8u; index++) {
        const uint32_t word = 4u + index * 4u;
        append_format(builder,
                      "    result.varying%u = float4(vita_output[%u], "
                      "vita_output[%u], vita_output[%u], vita_output[%u]);\n",
                      index, word, word + 1u, word + 2u, word + 3u);
    }
    append_format(builder, "    return result;\n}\n");
}

static void emit_compute_footer(hlsl_builder *builder) {
    append_format(builder,
                  "    [unroll] for (uint vita_i = 0u; vita_i < 128u; "
                  "vita_i++) {\n"
                  "        vita_state[vita_state_base + %uu + vita_i] = "
                  "vita_temporary[vita_i];\n"
                  "        vita_state[vita_state_base + %uu + vita_i] = "
                  "vita_output[vita_i];\n"
                  "    }\n"
                  "    [unroll] for (uint vita_i = 0u; vita_i < 4u; "
                  "vita_i++) {\n"
                  "        vita_state[vita_state_base + %uu + "
                  "vita_i * 4u + 0u] = "
                  "vita_internal[vita_i].x;\n"
                  "        vita_state[vita_state_base + %uu + "
                  "vita_i * 4u + 1u] = "
                  "vita_internal[vita_i].y;\n"
                  "        vita_state[vita_state_base + %uu + "
                  "vita_i * 4u + 2u] = "
                  "vita_internal[vita_i].z;\n"
                  "        vita_state[vita_state_base + %uu + "
                  "vita_i * 4u + 3u] = "
                  "vita_internal[vita_i].w;\n"
                  "    }\n"
                  "}\n",
                  VITA_USSE_HLSL_STATE_TEMPORARY_OFFSET,
                  VITA_USSE_HLSL_STATE_OUTPUT_OFFSET,
                  VITA_USSE_HLSL_STATE_INTERNAL_OFFSET,
                  VITA_USSE_HLSL_STATE_INTERNAL_OFFSET,
                  VITA_USSE_HLSL_STATE_INTERNAL_OFFSET,
                  VITA_USSE_HLSL_STATE_INTERNAL_OFFSET);
}

static vita_usse_translate_result
fail_translate(vita_usse_translate_failure *failure,
               vita_usse_translate_result result,
               const vita_usse_decode_failure *decode, const char *field) {
    if (failure) {
        memset(failure, 0, sizeof(*failure));
        failure->result = result;
        failure->field = field;
        if (decode)
            failure->decode = *decode;
        else
            failure->decode.instruction_index = UINT32_MAX;
    }
    return result;
}

static vita_usse_translate_result
emit_program(const vita_usse_stream_view *primary,
             const vita_usse_hlsl_options *options, hlsl_builder *builder,
             vita_usse_translate_failure *failure) {
    uint32_t emit_seen = 0u;
    uint32_t phase_seen = 0u;
    uint32_t arithmetic_seen = 0u;

    if (options->mode == VITA_USSE_HLSL_VERTEX)
        emit_vertex_header(builder);
    else
        emit_compute_header(builder);

    for (uint32_t index = 0u; index < primary->instruction_count; index++) {
        vita_usse_decoded_instruction instruction;
        vita_usse_decode_failure decode_failure;
        vita_usse_decode_result decode_result = vita_usse_decode_word(
            read_u64_le(primary->bytes + (size_t)index * 8u),
            VITA_USSE_PHASE_PRIMARY, &instruction, &decode_failure);
        if (decode_result != VITA_USSE_DECODE_OK) {
            decode_failure.instruction_index = index;
            return fail_translate(failure, VITA_USSE_TRANSLATE_DECODE_ERROR,
                                  &decode_failure, decode_failure.field);
        }
        if (emit_seen != 0u) {
            decode_failure = (vita_usse_decode_failure){
                VITA_USSE_DECODE_UNSUPPORTED_CONTROL,
                VITA_USSE_PHASE_PRIMARY,
                index,
                instruction.family,
                "instruction_after_emit",
            };
            return fail_translate(failure,
                                  VITA_USSE_TRANSLATE_INVALID_PROGRAM_ORDER,
                                  &decode_failure, "instruction_after_emit");
        }

        switch (instruction.operation) {
        case VITA_USSE_SEMANTIC_PHASE:
            if (phase_seen != 0u || arithmetic_seen != 0u) {
                decode_failure = (vita_usse_decode_failure){
                    VITA_USSE_DECODE_UNSUPPORTED_CONTROL,
                    VITA_USSE_PHASE_PRIMARY,
                    index,
                    instruction.family,
                    "phase_order",
                };
                return fail_translate(failure,
                                      VITA_USSE_TRANSLATE_INVALID_PROGRAM_ORDER,
                                      &decode_failure, "phase_order");
            }
            phase_seen = 1u;
            append_format(builder,
                          "    // PHAS wait=%u temporary_count=%u rate=%u:%u\n",
                          instruction.phase_control.wait_condition,
                          instruction.phase_control.temporary_count,
                          instruction.phase_control.rate_high,
                          instruction.phase_control.rate_low_or_no_schedule);
            break;
        case VITA_USSE_SEMANTIC_NOP:
            append_format(builder, "    // NOP\n");
            break;
        case VITA_USSE_SEMANTIC_EMIT:
            emit_seen = 1u;
            append_format(
                builder, "    // EMIT target=%u end=%u task_end=%u free=%u\n",
                instruction.emit_control.target, instruction.emit_control.end,
                instruction.emit_control.task_end,
                instruction.emit_control.free_partition);
            break;
        case VITA_USSE_SEMANTIC_PACK:
            arithmetic_seen = 1u;
            if (!emit_pack(builder, &instruction)) {
                decode_failure = (vita_usse_decode_failure){
                    VITA_USSE_DECODE_INVALID_REGISTER,
                    VITA_USSE_PHASE_PRIMARY,
                    index,
                    instruction.family,
                    "operand_range",
                };
                return fail_translate(failure,
                                      VITA_USSE_TRANSLATE_UNSUPPORTED_SEMANTIC,
                                      &decode_failure, "operand_range");
            }
            break;
        case VITA_USSE_SEMANTIC_DDX:
        case VITA_USSE_SEMANTIC_DDY:
            decode_failure = (vita_usse_decode_failure){
                VITA_USSE_DECODE_UNSUPPORTED_CONTROL,
                VITA_USSE_PHASE_PRIMARY,
                index,
                instruction.family,
                "derivative_operation",
            };
            return fail_translate(failure,
                                  VITA_USSE_TRANSLATE_UNSUPPORTED_SEMANTIC,
                                  &decode_failure, "derivative_operation");
        default:
            arithmetic_seen = 1u;
            if (!emit_alu(builder, &instruction)) {
                decode_failure = (vita_usse_decode_failure){
                    VITA_USSE_DECODE_INVALID_REGISTER,
                    VITA_USSE_PHASE_PRIMARY,
                    index,
                    instruction.family,
                    "operand_or_operation",
                };
                return fail_translate(failure,
                                      VITA_USSE_TRANSLATE_UNSUPPORTED_SEMANTIC,
                                      &decode_failure, "operand_or_operation");
            }
            break;
        }
        if (builder->failed)
            return fail_translate(failure, VITA_USSE_TRANSLATE_INTERNAL_ERROR,
                                  NULL, "source_builder");
    }

    if (emit_seen == 0u) {
        vita_usse_decode_failure decode_failure = {
            VITA_USSE_DECODE_UNSUPPORTED_CONTROL,
            VITA_USSE_PHASE_PRIMARY,
            primary->instruction_count,
            VITA_USSE_OPCODE_UNKNOWN,
            "missing_emit",
        };
        return fail_translate(failure,
                              VITA_USSE_TRANSLATE_INVALID_PROGRAM_ORDER,
                              &decode_failure, "missing_emit");
    }

    if (options->mode == VITA_USSE_HLSL_VERTEX)
        emit_vertex_footer(builder);
    else
        emit_compute_footer(builder);
    if (builder->failed)
        return fail_translate(failure, VITA_USSE_TRANSLATE_INTERNAL_ERROR, NULL,
                              "source_builder");
    return VITA_USSE_TRANSLATE_OK;
}

vita_usse_translate_result
vita_usse_translate_hlsl(const vita_usse_stream_view *primary,
                         const vita_usse_stream_view *secondary,
                         const vita_usse_hlsl_options *options, char *output,
                         size_t output_capacity, size_t *required_size,
                         vita_usse_translate_failure *failure) {
    hlsl_builder measure = {0};
    vita_usse_translate_result result;
    char *scratch;

    if (!primary || !secondary || !options || !required_size ||
        (!output && output_capacity != 0u) ||
        (options->mode != VITA_USSE_HLSL_VERTEX &&
         options->mode != VITA_USSE_HLSL_COMPUTE_CONFORMANCE) ||
        options->abi_version != VITA_USSE_HLSL_ABI_VERSION ||
        options->feature_mask != 0u)
        return fail_translate(failure, VITA_USSE_TRANSLATE_INVALID_ARGUMENT,
                              NULL, "argument");
    if (!validate_stream(primary) || !validate_stream(secondary))
        return fail_translate(failure, VITA_USSE_TRANSLATE_STREAM_OUT_OF_RANGE,
                              NULL, "stream");
    if (secondary->instruction_count != 0u)
        return fail_translate(failure, VITA_USSE_TRANSLATE_UNSUPPORTED_SEMANTIC,
                              NULL, "secondary_program");

    *required_size = 0u;
    result = emit_program(primary, options, &measure, failure);
    if (result != VITA_USSE_TRANSLATE_OK)
        return result;
    if (measure.length == SIZE_MAX)
        return fail_translate(failure, VITA_USSE_TRANSLATE_INTERNAL_ERROR, NULL,
                              "source_size");
    *required_size = measure.length + 1u;
    if (!output) {
        if (failure)
            memset(failure, 0, sizeof(*failure));
        return VITA_USSE_TRANSLATE_OK;
    }
    if (output_capacity < *required_size)
        return fail_translate(failure, VITA_USSE_TRANSLATE_OUTPUT_TOO_SMALL,
                              NULL, "output_capacity");

    scratch = (char *)malloc(*required_size);
    if (!scratch)
        return fail_translate(failure, VITA_USSE_TRANSLATE_INTERNAL_ERROR, NULL,
                              "allocation");
    hlsl_builder emit = {scratch, *required_size, 0u, 0};
    result = emit_program(primary, options, &emit, failure);
    if (result == VITA_USSE_TRANSLATE_OK) {
        scratch[emit.length] = '\0';
        memcpy(output, scratch, emit.length + 1u);
        if (failure)
            memset(failure, 0, sizeof(*failure));
    }
    free(scratch);
    return result;
}
