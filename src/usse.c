/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vitarecomp/usse.h"

#include <stdint.h>

/*
 * These masks express only public SGX543 USSE top-level encoding facts. They
 * classify a normalized numeric word and do not decode operands, prove that a
 * SPEC subopcode is valid, or perform semantic translation.
 * Factual references:
 *   https://wiki.henkaku.xyz/vita/SGX543
 *   https://github.com/Vita3K/Vita3K/blob/6d0393b5e7a1add2a380c1de87ad1fa87ace0242/tools/usse-decoder-gen/grammar.yaml
 */
typedef struct vita_usse_family_match {
    uint64_t mask;
    uint64_t value;
    vita_usse_opcode_family family;
} vita_usse_family_match;

static const vita_usse_family_match FAMILY_MATCHES[] = {
    {UINT64_C(0xF800000000000000), UINT64_C(0x0000000000000000),
     VITA_USSE_OPCODE_VMAD2},
    {UINT64_C(0xF800000000000000), UINT64_C(0x0800000000000000),
     VITA_USSE_OPCODE_V32NMAD},
    {UINT64_C(0xF800000000000000), UINT64_C(0x1000000000000000),
     VITA_USSE_OPCODE_V16NMAD},
    {UINT64_C(0xF820000000000000), UINT64_C(0x1820000000000000),
     VITA_USSE_OPCODE_VMAD},
    {UINT64_C(0xF820000000000000), UINT64_C(0x1800000000000000),
     VITA_USSE_OPCODE_VDP},
    {UINT64_C(0xF000000000000000), UINT64_C(0x2000000000000000),
     VITA_USSE_OPCODE_VDUAL},
    {UINT64_C(0xF800000000000000), UINT64_C(0x3000000000000000),
     VITA_USSE_OPCODE_VCOMP},
    {UINT64_C(0xF800000000000000), UINT64_C(0x3800000000000000),
     VITA_USSE_OPCODE_VMOV},
    {UINT64_C(0xF800000000000000), UINT64_C(0x4000000000000000),
     VITA_USSE_OPCODE_VPCK},
    {UINT64_C(0xF800000000000000), UINT64_C(0x4800000000000000),
     VITA_USSE_OPCODE_VTST},
    {UINT64_C(0xF800000000000000), UINT64_C(0x7800000000000000),
     VITA_USSE_OPCODE_VTSTMSK},
    {UINT64_C(0xC000000000000000), UINT64_C(0x4000000000000000),
     VITA_USSE_OPCODE_VBW},
    {UINT64_C(0xF800000000000000), UINT64_C(0x8000000000000000),
     VITA_USSE_OPCODE_SOP2},
    {UINT64_C(0xF800000000000000), UINT64_C(0x9000000000000000),
     VITA_USSE_OPCODE_SOP2M},
    {UINT64_C(0xF800000000000000), UINT64_C(0x8800000000000000),
     VITA_USSE_OPCODE_SOP3},
    {UINT64_C(0xF800000000000000), UINT64_C(0x9800000000000000),
     VITA_USSE_OPCODE_I8MAD},
    {UINT64_C(0xF800000000000000), UINT64_C(0xA000000000000000),
     VITA_USSE_OPCODE_I16MAD},
    {UINT64_C(0xF800833800000000), UINT64_C(0xA800000000000000),
     VITA_USSE_OPCODE_I32MAD},
    {UINT64_C(0xF800000000000000), UINT64_C(0xB000000000000000),
     VITA_USSE_OPCODE_ILLEGAL22},
    {UINT64_C(0xF800000000000000), UINT64_C(0xB800000000000000),
     VITA_USSE_OPCODE_ILLEGAL23},
    {UINT64_C(0xF800000000000000), UINT64_C(0xC000000000000000),
     VITA_USSE_OPCODE_ILLEGAL24},
    {UINT64_C(0xF800000000000000), UINT64_C(0xC800000000000000),
     VITA_USSE_OPCODE_I8MAD2},
    {UINT64_C(0xF8000C7800000000), UINT64_C(0xD000000000000000),
     VITA_USSE_OPCODE_I32MAD2},
    {UINT64_C(0xF800000000000000), UINT64_C(0xD800000000000000),
     VITA_USSE_OPCODE_ILLEGAL27},
    {UINT64_C(0xF800000000000000), UINT64_C(0xE000000000000000),
     VITA_USSE_OPCODE_SMP},
    {UINT64_C(0xFF70000000000000), UINT64_C(0xFA40000000000000),
     VITA_USSE_OPCODE_PHAS},
    {UINT64_C(0xF87001C000000000), UINT64_C(0xF800014000000000),
     VITA_USSE_OPCODE_NOP},
    {UINT64_C(0xF870018000000000), UINT64_C(0xF800000000000000),
     VITA_USSE_OPCODE_BR},
    {UINT64_C(0xFF30000000000000), UINT64_C(0xFA10000000000000),
     VITA_USSE_OPCODE_SMLSI},
    {UINT64_C(0xFF30000000000000), UINT64_C(0xFB10000000000000),
     VITA_USSE_OPCODE_SMBO},
    {UINT64_C(0xFF3FF9FFF0000000), UINT64_C(0xF9300006F0000000),
     VITA_USSE_OPCODE_KILL},
    {UINT64_C(0xFF30000000000000), UINT64_C(0xFC20000000000000),
     VITA_USSE_OPCODE_LIMM},
    {UINT64_C(0xFF30000000000000), UINT64_C(0xFB30000000000000),
     VITA_USSE_OPCODE_DEPTHF},
    {UINT64_C(0xF800000000000000), UINT64_C(0xF800000000000000),
     VITA_USSE_OPCODE_SPEC},
    {UINT64_C(0xE000000000000000), UINT64_C(0xE000000000000000),
     VITA_USSE_OPCODE_VLDST},
};

static const char *const FAMILY_NAMES[] = {
    "UNKNOWN",   "VMAD2",     "V32NMAD", "V16NMAD", "VMAD",
    "VDP",       "VDUAL",     "VCOMP",   "VMOV",    "VPCK",
    "VTST",      "VTSTMSK",   "VBW",     "SOP2",    "SOP2M",
    "SOP3",      "I8MAD",     "I16MAD",  "I32MAD",  "ILLEGAL22",
    "ILLEGAL23", "ILLEGAL24", "I8MAD2",  "I32MAD2", "ILLEGAL27",
    "SMP",       "PHAS",      "NOP",     "BR",      "SMLSI",
    "SMBO",      "KILL",      "LIMM",    "DEPTHF",  "SPEC",
    "VLDST",
};

_Static_assert(
    sizeof(FAMILY_MATCHES) / sizeof(FAMILY_MATCHES[0]) ==
        (size_t)VITA_USSE_OPCODE_FAMILY_COUNT - 1u,
    "every non-UNKNOWN USSE family must have one matcher");
_Static_assert(
    sizeof(FAMILY_NAMES) / sizeof(FAMILY_NAMES[0]) ==
        (size_t)VITA_USSE_OPCODE_FAMILY_COUNT,
    "every USSE family must have one static name");

static uint64_t read_u64_le(const uint8_t *bytes) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8u; i++)
        value |= (uint64_t)bytes[i] << (i * 8u);
    return value;
}

vita_usse_opcode_family vita_usse_classify_word(uint64_t instruction) {
    for (size_t i = 0; i < sizeof(FAMILY_MATCHES) / sizeof(FAMILY_MATCHES[0]);
         i++) {
        const vita_usse_family_match *match = &FAMILY_MATCHES[i];
        if ((instruction & match->mask) == match->value)
            return match->family;
    }
    return VITA_USSE_OPCODE_UNKNOWN;
}

static int family_is_illegal(vita_usse_opcode_family family) {
    return family == VITA_USSE_OPCODE_ILLEGAL22 ||
           family == VITA_USSE_OPCODE_ILLEGAL23 ||
           family == VITA_USSE_OPCODE_ILLEGAL24 ||
           family == VITA_USSE_OPCODE_ILLEGAL27;
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
            vita_usse_classify_word(
                read_u64_le(stream->bytes + (size_t)i * 8u));
        census->total++;
        census->family_counts[family]++;
        if (family == VITA_USSE_OPCODE_UNKNOWN) {
            census->unclassified++;
            if (census->first_unclassified_phase == VITA_USSE_PHASE_NONE) {
                census->first_unclassified_phase = phase;
                census->first_unclassified_index = i;
                census->first_unclassified_family = family;
            }
        } else {
            census->classified++;
            if (family_is_illegal(family)) {
                census->illegal++;
                if (census->first_illegal_phase == VITA_USSE_PHASE_NONE) {
                    census->first_illegal_phase = phase;
                    census->first_illegal_index = i;
                    census->first_illegal_family = family;
                }
            }
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
    measured.first_unclassified_phase = VITA_USSE_PHASE_NONE;
    measured.first_unclassified_index = UINT32_MAX;
    measured.first_unclassified_family = VITA_USSE_OPCODE_UNKNOWN;
    measured.first_illegal_phase = VITA_USSE_PHASE_NONE;
    measured.first_illegal_index = UINT32_MAX;
    measured.first_illegal_family = VITA_USSE_OPCODE_UNKNOWN;

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
    const uint32_t family_index = (uint32_t)family;
    return family_index < (uint32_t)VITA_USSE_OPCODE_FAMILY_COUNT
        ? FAMILY_NAMES[family_index]
        : "INVALID";
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
