/* reloc.c -- shared PT_SCE_RELA decoder and identity relocation verifier. */

#include "vitarecomp/reloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELF32_HEADER_SIZE 0x34u
#define ELF32_PROGRAM_HEADER_SIZE 0x20u

typedef struct {
    uint32_t base;
    uint32_t memsz;
    uint32_t file;
    uint32_t filesz;
    int ok;
} seginfo;

typedef struct {
    uint32_t addr;
    uint32_t offset;
    uint32_t patch_seg;
    uint32_t saddr;
    uint32_t sym_seg;
    uint32_t addend;
    uint8_t type;
    uint8_t type2;
    int initialized;
} rstate;

static const uint8_t entry_size[10] = {12, 8, 8, 8, 4, 4, 4, 4, 4, 4};

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void wr32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int add_u32(uint32_t left, uint32_t right, uint32_t *out) {
    uint64_t sum = (uint64_t)left + right;
    if (sum > UINT32_MAX) return 0;
    *out = (uint32_t)sum;
    return 1;
}

static void clear_error(vita_reloc_error *error) {
    if (!error) return;
    memset(error, 0, sizeof(*error));
    error->status = VITA_RELOC_OK;
    error->program_header_index = VITA_RELOC_INDEX_NONE;
    error->record_index = VITA_RELOC_INDEX_NONE;
    error->stream_offset = VITA_RELOC_INDEX_NONE;
}

static vita_reloc_status fail(
    vita_reloc_error *error,
    vita_reloc_status status,
    uint32_t program_header_index,
    uint32_t record_index,
    uint32_t stream_offset,
    const char *format,
    ...) {
    if (error) {
        va_list args;
        clear_error(error);
        error->status = status;
        error->program_header_index = program_header_index;
        error->record_index = record_index;
        error->stream_offset = stream_offset;
        va_start(args, format);
        (void)vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return status;
}

static seginfo seg_lookup(const vita_reloc_module *module, uint32_t index) {
    seginfo segment;
    memset(&segment, 0, sizeof(segment));
    if (index < module->program_header_count &&
        module->program_headers[index].type == VITA_RELOC_PT_LOAD) {
        const vita_reloc_program_header *header =
            &module->program_headers[index];
        segment.base = header->virtual_address;
        segment.memsz = header->memory_size;
        segment.file = header->file_offset;
        segment.filesz = header->file_size;
        segment.ok = 1;
    }
    return segment;
}

static uint32_t sym_base(
    const vita_reloc_module *module,
    uint32_t sym_seg,
    vita_reloc_table *table) {
    seginfo segment;
    if (sym_seg == VITA_RELOC_SEGMENT_ABSOLUTE) return 0;
    segment = seg_lookup(module, sym_seg);
    if (!segment.ok) {
        table->bad_segment++;
        return 0;
    }
    return segment.base;
}

static int push(
    vita_reloc_table *table,
    uint32_t patch,
    uint32_t value,
    uint8_t format,
    uint8_t code,
    uint8_t patch_seg,
    uint8_t sym_seg) {
    vita_reloc_entry *entries;
    uint32_t capacity;
    vita_reloc_entry *entry;
    if (code == VITA_RELOC_NONE) return 0;
    if (table->n == table->cap) {
        if (table->cap > UINT32_MAX / 2u) return 1;
        capacity = table->cap ? table->cap * 2u : 4096u;
        if ((size_t)capacity > SIZE_MAX / sizeof(*entries)) return 1;
        entries = (vita_reloc_entry *)realloc(
            table->v, (size_t)capacity * sizeof(*entries));
        if (!entries) return 1;
        table->v = entries;
        table->cap = capacity;
    }
    entry = &table->v[table->n++];
    entry->patch = patch;
    entry->value = value;
    entry->format = format;
    entry->code = code;
    entry->patch_seg = patch_seg;
    entry->sym_seg = sym_seg;
    table->by_code[code]++;
    return 0;
}

static int rebase(
    const uint8_t *bytes,
    size_t size,
    const vita_reloc_module *module,
    rstate *state,
    vita_reloc_table *table,
    uint8_t format) {
    seginfo patch_segment = seg_lookup(module, state->patch_seg);
    uint64_t file_offset;
    uint32_t original;
    uint32_t symbol_segment = VITA_RELOC_SEGMENT_ABSOLUTE;
    uint32_t patch;
    uint32_t index;
    if (!patch_segment.ok) {
        table->bad_segment++;
        return 0;
    }
    file_offset = (uint64_t)patch_segment.file + state->offset;
    if ((uint64_t)state->offset + 4u > patch_segment.filesz ||
        file_offset + 4u > size) {
        table->rebase_unmatched++;
        return 0;
    }
    original = rd32(bytes + file_offset);
    for (index = 0; index < module->program_header_count; index++) {
        const vita_reloc_program_header *header =
            &module->program_headers[index];
        uint64_t begin;
        uint64_t end;
        if (header->type != VITA_RELOC_PT_LOAD) continue;
        begin = header->virtual_address;
        end = begin + header->memory_size;
        if ((uint64_t)original >= begin && (uint64_t)original < end)
            symbol_segment = index;
    }
    if (symbol_segment == VITA_RELOC_SEGMENT_ABSOLUTE) {
        table->rebase_unmatched++;
    } else {
        state->saddr =
            module->program_headers[symbol_segment].virtual_address;
    }
    state->sym_seg = symbol_segment;
    state->type = VITA_RELOC_ABS32;
    state->type2 = VITA_RELOC_NONE;
    if (!add_u32(patch_segment.base, state->offset, &patch)) return -1;
    return push(table, patch, original, format, VITA_RELOC_ABS32,
                (uint8_t)state->patch_seg, (uint8_t)symbol_segment);
}

static vita_reloc_status parse_stream(
    const uint8_t *bytes,
    size_t size,
    const vita_reloc_module *module,
    const uint8_t *stream,
    uint32_t length,
    uint32_t program_header_index,
    vita_reloc_table *table,
    vita_reloc_error *error) {
    rstate state;
    uint32_t position = 0;
    uint32_t record_index = 0;
    memset(&state, 0, sizeof(state));

    while (position < length) {
        uint32_t word0;
        uint32_t format;
        uint32_t word1;
        uint32_t word2;
        int oom = 0;
        if (length - position < 4u) {
            table->trailing += length - position;
            return fail(error, VITA_RELOC_ERROR_TRUNCATED_RECORD,
                        program_header_index, record_index, position,
                        "truncated relocation record at stream offset %u",
                        position);
        }
        word0 = rd32(stream + position);
        format = word0 & 0xFu;
        if (format > 9u)
            return fail(error, VITA_RELOC_ERROR_UNKNOWN_FORMAT,
                        program_header_index, record_index, position,
                        "unknown relocation entry format %u at stream offset %u",
                        format, position);
        if (length - position < entry_size[format]) {
            table->trailing += length - position;
            return fail(error, VITA_RELOC_ERROR_TRUNCATED_RECORD,
                        program_header_index, record_index, position,
                        "truncated relocation format %u at stream offset %u",
                        format, position);
        }
        if (format >= 2u && !state.initialized)
            return fail(error, VITA_RELOC_ERROR_MISSING_STATE,
                        program_header_index, record_index, position,
                        "relocation format %u has no preceding state at stream offset %u",
                        format, position);
        table->by_format[format]++;
        word1 = entry_size[format] >= 8u ? rd32(stream + position + 4u) : 0u;
        word2 = entry_size[format] >= 12u ? rd32(stream + position + 8u) : 0u;

        switch (format) {
        case 0: {
            uint32_t sym = (word0 >> 4) & 0xFu;
            uint8_t code = (uint8_t)((word0 >> 8) & 0xFFu);
            uint32_t patch_segment_index = (word0 >> 16) & 0xFu;
            uint8_t code2 = (uint8_t)((word0 >> 20) & 0xFFu);
            uint32_t distance2 = (word0 >> 28) & 0xFu;
            uint32_t addend = word1;
            uint32_t offset = word2;
            seginfo patch_segment =
                seg_lookup(module, patch_segment_index);
            uint32_t symbol_base;
            uint32_t patch;
            uint32_t second = 0;
            uint32_t value;
            if (!patch_segment.ok) table->bad_segment++;
            symbol_base = sym_base(module, sym, table);
            if (!add_u32(patch_segment.base, offset, &patch) ||
                (code2 != VITA_RELOC_NONE &&
                 !add_u32(patch, distance2 * 2u, &second)))
                return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                            program_header_index, record_index, position,
                            "relocation format 0 address overflow at stream offset %u",
                            position);
            value = symbol_base + addend;
            oom |= push(table, patch, value, 0, code,
                        (uint8_t)patch_segment_index, (uint8_t)sym);
            if (code2 != VITA_RELOC_NONE)
                oom |= push(table, second, value, 0, code2,
                            (uint8_t)patch_segment_index, (uint8_t)sym);
            state.addr = patch_segment.base;
            state.offset = offset;
            state.patch_seg = patch_segment_index;
            state.saddr = symbol_base;
            state.sym_seg = sym;
            state.addend = addend;
            state.type = code;
            state.type2 = code2;
            state.initialized = 1;
            break;
        }
        case 1: {
            uint32_t sym = (word0 >> 4) & 0xFu;
            uint8_t code = (uint8_t)((word0 >> 8) & 0xFFu);
            uint32_t patch_segment_index = (word0 >> 16) & 0xFu;
            uint32_t offset =
                ((word0 >> 20) & 0xFFFu) | ((word1 & 0x3FFu) << 12);
            uint32_t addend = word1 >> 10;
            seginfo patch_segment =
                seg_lookup(module, patch_segment_index);
            uint32_t symbol_base;
            uint32_t patch;
            if (!patch_segment.ok) table->bad_segment++;
            symbol_base = sym_base(module, sym, table);
            if (!add_u32(patch_segment.base, offset, &patch))
                return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                            program_header_index, record_index, position,
                            "relocation format 1 address overflow at stream offset %u",
                            position);
            oom |= push(table, patch, symbol_base + addend, 1, code,
                        (uint8_t)patch_segment_index, (uint8_t)sym);
            state.addr = patch_segment.base;
            state.offset = offset;
            state.patch_seg = patch_segment_index;
            state.saddr = symbol_base;
            state.sym_seg = sym;
            state.addend = addend;
            state.type = code;
            state.type2 = VITA_RELOC_NONE;
            state.initialized = 1;
            break;
        }
        case 2: {
            uint32_t sym = (word0 >> 4) & 0xFu;
            uint8_t code = (uint8_t)((word0 >> 8) & 0xFFu);
            uint32_t offset = (word0 >> 16) & 0xFFFFu;
            uint32_t patch;
            if (!add_u32(state.offset, offset, &state.offset))
                return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                            program_header_index, record_index, position,
                            "relocation format 2 running offset overflow at stream offset %u",
                            position);
            state.saddr = sym_base(module, sym, table);
            state.sym_seg = sym;
            state.addend = word1;
            state.type = code;
            state.type2 = VITA_RELOC_NONE;
            if (!add_u32(state.addr, state.offset, &patch))
                return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                            program_header_index, record_index, position,
                            "relocation format 2 address overflow at stream offset %u",
                            position);
            oom |= push(table, patch, state.saddr + state.addend, 2, code,
                        (uint8_t)state.patch_seg, (uint8_t)sym);
            break;
        }
        case 3: {
            uint32_t sym = (word0 >> 4) & 0xFu;
            uint32_t mode = (word0 >> 8) & 1u;
            uint32_t offset = (word0 >> 9) & 0x3FFFFu;
            uint32_t distance2 = (word0 >> 27) & 0x1Fu;
            uint32_t patch;
            uint32_t second;
            uint32_t value;
            state.type = (uint8_t)(mode ? VITA_RELOC_THM_MOVW_ABS_NC
                                       : VITA_RELOC_MOVW_ABS_NC);
            state.type2 = (uint8_t)(mode ? VITA_RELOC_THM_MOVT_ABS
                                        : VITA_RELOC_MOVT_ABS);
            if (!add_u32(state.offset, offset, &state.offset) ||
                !add_u32(state.addr, state.offset, &patch) ||
                !add_u32(patch, distance2, &second))
                return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                            program_header_index, record_index, position,
                            "relocation format 3 address overflow at stream offset %u",
                            position);
            state.saddr = sym_base(module, sym, table);
            state.sym_seg = sym;
            state.addend = word1 & 0x3FFFFFu;
            value = state.saddr + state.addend;
            oom |= push(table, patch, value, 3, state.type,
                        (uint8_t)state.patch_seg, (uint8_t)sym);
            oom |= push(table, second, value, 3, state.type2,
                        (uint8_t)state.patch_seg, (uint8_t)sym);
            break;
        }
        case 4: {
            uint32_t offset = (word0 >> 4) & 0x7FFFFFu;
            uint32_t distance2 = (word0 >> 27) & 0x1Fu;
            uint32_t patch;
            uint32_t second;
            uint32_t value;
            if (!add_u32(state.offset, offset, &state.offset) ||
                !add_u32(state.addr, state.offset, &patch) ||
                !add_u32(patch, distance2, &second))
                return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                            program_header_index, record_index, position,
                            "relocation format 4 address overflow at stream offset %u",
                            position);
            value = state.saddr + state.addend;
            oom |= push(table, patch, value, 4, state.type,
                        (uint8_t)state.patch_seg, (uint8_t)state.sym_seg);
            oom |= push(table, second, value, 4, state.type2,
                        (uint8_t)state.patch_seg, (uint8_t)state.sym_seg);
            break;
        }
        case 5: {
            uint32_t distance1 = (word0 >> 4) & 0x1FFu;
            uint32_t distance2 = (word0 >> 13) & 0x1Fu;
            uint32_t distance3 = (word0 >> 18) & 0x1FFu;
            uint32_t distance4 = (word0 >> 27) & 0x1Fu;
            uint32_t patch;
            uint32_t second;
            uint32_t third;
            uint32_t fourth;
            uint32_t value;
            if (!add_u32(state.offset, distance1, &state.offset) ||
                !add_u32(state.addr, state.offset, &patch) ||
                !add_u32(patch, distance2, &second) ||
                !add_u32(state.offset, distance3, &state.offset) ||
                !add_u32(state.addr, state.offset, &third) ||
                !add_u32(third, distance4, &fourth))
                return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                            program_header_index, record_index, position,
                            "relocation format 5 address overflow at stream offset %u",
                            position);
            value = state.saddr + state.addend;
            oom |= push(table, patch, value, 5, state.type,
                        (uint8_t)state.patch_seg, (uint8_t)state.sym_seg);
            oom |= push(table, second, value, 5, state.type2,
                        (uint8_t)state.patch_seg, (uint8_t)state.sym_seg);
            oom |= push(table, third, value, 5, state.type,
                        (uint8_t)state.patch_seg, (uint8_t)state.sym_seg);
            oom |= push(table, fourth, value, 5, state.type2,
                        (uint8_t)state.patch_seg, (uint8_t)state.sym_seg);
            break;
        }
        case 6: {
            int result;
            if (!add_u32(state.offset, word0 >> 4, &state.offset))
                return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                            program_header_index, record_index, position,
                            "relocation format 6 running offset overflow at stream offset %u",
                            position);
            result = rebase(bytes, size, module, &state, table, 6);
            if (result < 0)
                return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                            program_header_index, record_index, position,
                            "relocation format 6 address overflow at stream offset %u",
                            position);
            oom |= result;
            break;
        }
        case 7:
        case 8:
        case 9: {
            uint32_t offsets = word0 >> 4;
            uint32_t bits = format == 7u ? 7u : format == 8u ? 4u : 2u;
            uint32_t mask = (1u << bits) - 1u;
            do {
                int result;
                if (!add_u32(state.offset, (offsets & mask) * 4u,
                             &state.offset))
                    return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                                program_header_index, record_index, position,
                                "relocation format %u running offset overflow at stream offset %u",
                                format, position);
                result = rebase(bytes, size, module, &state, table,
                                (uint8_t)format);
                if (result < 0)
                    return fail(error, VITA_RELOC_ERROR_ADDRESS_OVERFLOW,
                                program_header_index, record_index, position,
                                "relocation format %u address overflow at stream offset %u",
                                format, position);
                oom |= result;
            } while ((offsets >>= bits) != 0u);
            break;
        }
        default:
            break;
        }
        if (oom)
            return fail(error, VITA_RELOC_ERROR_NO_MEMORY,
                        program_header_index, record_index, position,
                        "out of memory decoding relocations");
        position += entry_size[format];
        record_index++;
    }
    return VITA_RELOC_OK;
}

vita_reloc_status vita_reloc_parse(
    const uint8_t *bytes,
    size_t size,
    const vita_reloc_module *module,
    vita_reloc_table *out,
    vita_reloc_error *error) {
    uint32_t index;
    if (!out) return VITA_RELOC_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    clear_error(error);
    if (!bytes || !module ||
        (module->program_header_count && !module->program_headers))
        return fail(error, VITA_RELOC_ERROR_INVALID_ARGUMENT,
                    VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                    VITA_RELOC_INDEX_NONE,
                    "invalid relocation decoder argument");

    for (index = 0; index < module->program_header_count; index++) {
        const vita_reloc_program_header *header =
            &module->program_headers[index];
        if (header->type != VITA_RELOC_PT_LOAD) continue;
        if ((uint64_t)header->virtual_address + header->memory_size >
            UINT64_C(0x100000000))
            return fail(error, VITA_RELOC_ERROR_VIRTUAL_RANGE, index,
                        VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                        "load segment %u virtual range overflows 32-bit address space",
                        index);
        if ((uint64_t)header->file_offset + header->file_size > size)
            return fail(error, VITA_RELOC_ERROR_FILE_RANGE, index,
                        VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                        "load segment %u extends past the file", index);
    }
    for (index = 0; index < module->program_header_count; index++) {
        const vita_reloc_program_header *header =
            &module->program_headers[index];
        vita_reloc_status status;
        if (header->type != VITA_RELOC_PT_SCE_RELA) continue;
        out->streams++;
        if (UINT32_MAX - out->stream_bytes < header->file_size)
            return fail(error, VITA_RELOC_ERROR_FILE_RANGE, index,
                        VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                        "relocation stream byte count overflows");
        out->stream_bytes += header->file_size;
        if (!header->file_size)
            return fail(error, VITA_RELOC_ERROR_TRUNCATED_RECORD, index, 0u,
                        0u, "relocation segment %u is empty", index);
        if ((uint64_t)header->file_offset + header->file_size > size)
            return fail(error, VITA_RELOC_ERROR_FILE_RANGE, index,
                        VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                        "relocation segment %u extends past the file", index);
        status = parse_stream(bytes, size, module,
                              bytes + header->file_offset, header->file_size,
                              index, out, error);
        if (status != VITA_RELOC_OK) return status;
    }
    return VITA_RELOC_OK;
}

static vita_reloc_status extract_elf_module(
    const uint8_t *bytes,
    size_t size,
    vita_reloc_program_header **out_headers,
    vita_reloc_module *out_module,
    vita_reloc_error *error) {
    uint32_t program_offset;
    uint16_t program_size;
    uint16_t program_count;
    vita_reloc_program_header *headers = NULL;
    uint32_t index;
    *out_headers = NULL;
    memset(out_module, 0, sizeof(*out_module));
    if (!bytes)
        return fail(error, VITA_RELOC_ERROR_INVALID_ARGUMENT,
                    VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                    VITA_RELOC_INDEX_NONE, "ELF bytes are null");
    if (size < ELF32_HEADER_SIZE || bytes[0] != 0x7Fu ||
        bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F' ||
        bytes[4] != 1u || bytes[5] != 1u)
        return fail(error, VITA_RELOC_ERROR_ELF_HEADER,
                    VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                    VITA_RELOC_INDEX_NONE,
                    "relocation input is not little-endian ELF32");
    program_offset = rd32(bytes + 0x1Cu);
    program_size = rd16(bytes + 0x2Au);
    program_count = rd16(bytes + 0x2Cu);
    if (program_size != ELF32_PROGRAM_HEADER_SIZE ||
        (program_count &&
         ((uint64_t)program_offset +
              (uint64_t)program_count * ELF32_PROGRAM_HEADER_SIZE >
          size)))
        return fail(error, VITA_RELOC_ERROR_PROGRAM_HEADER_TABLE,
                    VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                    VITA_RELOC_INDEX_NONE,
                    "invalid ELF32 program-header table");
    if (program_count) {
        headers = (vita_reloc_program_header *)calloc(
            program_count, sizeof(*headers));
        if (!headers)
            return fail(error, VITA_RELOC_ERROR_NO_MEMORY,
                        VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                        VITA_RELOC_INDEX_NONE,
                        "out of memory extracting program headers");
    }
    for (index = 0; index < program_count; index++) {
        const uint8_t *header =
            bytes + program_offset + (size_t)index * program_size;
        headers[index].type = rd32(header + 0x00);
        headers[index].file_offset = rd32(header + 0x04);
        headers[index].virtual_address = rd32(header + 0x08);
        headers[index].file_size = rd32(header + 0x10);
        headers[index].memory_size = rd32(header + 0x14);
    }
    out_module->program_headers = headers;
    out_module->program_header_count = program_count;
    *out_headers = headers;
    return VITA_RELOC_OK;
}

vita_reloc_status vita_reloc_parse_elf(
    const uint8_t *bytes,
    size_t size,
    vita_reloc_table *out,
    vita_reloc_error *error) {
    vita_reloc_program_header *headers;
    vita_reloc_module module;
    vita_reloc_status status;
    if (!out) return VITA_RELOC_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    clear_error(error);
    status = extract_elf_module(bytes, size, &headers, &module, error);
    if (status != VITA_RELOC_OK) return status;
    status = vita_reloc_parse(bytes, size, &module, out, error);
    free(headers);
    return status;
}

static vita_reloc_status inspect_elf_identity(
    const uint8_t *bytes,
    size_t size,
    int require_identity,
    vita_reloc_table *out,
    vita_reloc_identity *identity,
    vita_reloc_error *error,
    vita_reloc_program_header **out_headers,
    vita_reloc_module *out_module) {
    vita_reloc_program_header *headers;
    vita_reloc_module module;
    vita_reloc_status status;
    uint32_t index;
    if (!out || !identity || !out_headers || !out_module)
        return fail(error, VITA_RELOC_ERROR_INVALID_ARGUMENT,
                    VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                    VITA_RELOC_INDEX_NONE,
                    "invalid identity relocation argument");
    memset(out, 0, sizeof(*out));
    memset(identity, 0, sizeof(*identity));
    *out_headers = NULL;
    memset(out_module, 0, sizeof(*out_module));
    clear_error(error);
    status = extract_elf_module(bytes, size, &headers, &module, error);
    if (status != VITA_RELOC_OK) return status;
    status = vita_reloc_parse(bytes, size, &module, out, error);
    if (status != VITA_RELOC_OK) {
        free(headers);
        return status;
    }
    if (out->bad_segment) {
        for (index = 0; index < out->n; index++) {
            const vita_reloc_entry *entry = &out->v[index];
            if (!seg_lookup(&module, entry->patch_seg).ok ||
                (entry->sym_seg != VITA_RELOC_SEGMENT_ABSOLUTE &&
                 !seg_lookup(&module, entry->sym_seg).ok))
                break;
        }
        free(headers);
        return fail(error, VITA_RELOC_ERROR_BAD_SEGMENT,
                    VITA_RELOC_INDEX_NONE,
                    index < out->n ? index : VITA_RELOC_INDEX_NONE,
                    VITA_RELOC_INDEX_NONE,
                    "relocation stream names a non-PT_LOAD segment");
    }
    if (out->rebase_unmatched) {
        free(headers);
        return fail(error, VITA_RELOC_ERROR_REBASE_TARGET,
                    VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                    VITA_RELOC_INDEX_NONE,
                    "rebase relocation target is outside every PT_LOAD segment");
    }
    status = vita_reloc_verify_identity(
        bytes, size, &module, out, identity);
    if (status != VITA_RELOC_OK)
        status = fail(error, status, VITA_RELOC_INDEX_NONE,
                      VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                      "identity relocation verification failed");
    else if (identity->unsupported)
        status = fail(error, VITA_RELOC_ERROR_UNSUPPORTED_CODE,
                    VITA_RELOC_INDEX_NONE, identity->first_unsupported,
                    VITA_RELOC_INDEX_NONE,
                    "unsupported relocation code at record %u",
                    identity->first_unsupported);
    else if (identity->outside)
        status = fail(error, VITA_RELOC_ERROR_PATCH_OUTSIDE_FILE,
                    VITA_RELOC_INDEX_NONE, identity->first_outside,
                    VITA_RELOC_INDEX_NONE,
                    "relocation patch is outside declared file bytes at record %u",
                    identity->first_outside);
    else if (require_identity && identity->changed)
        status = fail(error, VITA_RELOC_ERROR_NOT_IDENTITY,
                    VITA_RELOC_INDEX_NONE, identity->first_changed,
                    VITA_RELOC_INDEX_NONE,
                    "post-link bytes are not identity-relocated at record %u",
                    identity->first_changed);
    if (status != VITA_RELOC_OK) {
        free(headers);
        return status;
    }
    *out_headers = headers;
    *out_module = module;
    return VITA_RELOC_OK;
}

vita_reloc_status vita_reloc_validate_elf_identity(
    const uint8_t *bytes,
    size_t size,
    vita_reloc_table *out,
    vita_reloc_identity *identity,
    vita_reloc_error *error) {
    vita_reloc_program_header *headers;
    vita_reloc_module module;
    vita_reloc_status status = inspect_elf_identity(
        bytes, size, 1, out, identity, error, &headers, &module);
    free(headers);
    return status;
}

vita_reloc_status vita_reloc_normalize_elf_identity(
    uint8_t *bytes,
    size_t size,
    vita_reloc_table *out,
    vita_reloc_identity *identity,
    vita_reloc_error *error) {
    vita_reloc_program_header *headers;
    vita_reloc_module module;
    vita_reloc_identity before;
    vita_reloc_identity after;
    vita_reloc_status status = inspect_elf_identity(
        bytes, size, 0, out, &before, error, &headers, &module);
    if (status != VITA_RELOC_OK) return status;
    status = vita_reloc_apply(bytes, size, &module, out, &after);
    if (status == VITA_RELOC_OK)
        status = vita_reloc_verify_identity(
            bytes, size, &module, out, &after);
    free(headers);
    if (status != VITA_RELOC_OK)
        return fail(error, status, VITA_RELOC_INDEX_NONE,
                    VITA_RELOC_INDEX_NONE, VITA_RELOC_INDEX_NONE,
                    "identity relocation normalization failed");
    if (after.unsupported)
        return fail(error, VITA_RELOC_ERROR_UNSUPPORTED_CODE,
                    VITA_RELOC_INDEX_NONE, after.first_unsupported,
                    VITA_RELOC_INDEX_NONE,
                    "normalization left unsupported relocation code at record %u",
                    after.first_unsupported);
    if (after.outside)
        return fail(error, VITA_RELOC_ERROR_PATCH_OUTSIDE_FILE,
                    VITA_RELOC_INDEX_NONE, after.first_outside,
                    VITA_RELOC_INDEX_NONE,
                    "normalization left an out-of-file patch at record %u",
                    after.first_outside);
    if (after.changed)
        return fail(error, VITA_RELOC_ERROR_NOT_IDENTITY,
                    VITA_RELOC_INDEX_NONE, after.first_changed,
                    VITA_RELOC_INDEX_NONE,
                    "normalization did not materialize record %u",
                    after.first_changed);
    *identity = before;
    return VITA_RELOC_OK;
}

static void patch_thumb_mov(uint8_t *bytes, uint32_t value) {
    uint16_t high = rd16(bytes);
    uint16_t low = rd16(bytes + 2);
    high = (uint16_t)((high & ~0x040Fu) |
                      (((value >> 11) & 1u) << 10) |
                      ((value >> 12) & 0xFu));
    low = (uint16_t)((low & ~0x70FFu) |
                     (((value >> 8) & 7u) << 12) |
                     (value & 0xFFu));
    wr16(bytes, high);
    wr16(bytes + 2, low);
}

static void patch_arm_mov(uint8_t *bytes, uint32_t value) {
    uint32_t word = rd32(bytes);
    word = (word & ~0x000F0FFFu) | (value & 0xFFFu) |
           (((value >> 12) & 0xFu) << 16);
    wr32(bytes, word);
}

static void patch_thumb_call(uint8_t *bytes, uint32_t value) {
    uint16_t high = rd16(bytes);
    uint16_t low = rd16(bytes + 2);
    uint32_t sign = (value >> 24) & 1u;
    uint32_t j1 = sign ^ (~(value >> 23) & 1u);
    uint32_t j2 = sign ^ (~(value >> 22) & 1u);
    high = (uint16_t)((high & ~0x07FFu) | (sign << 10) |
                      ((value >> 12) & 0x3FFu));
    low = (uint16_t)((low & ~0x2FFFu) | (j1 << 13) | (j2 << 11) |
                     ((value >> 1) & 0x7FFu));
    wr16(bytes, high);
    wr16(bytes + 2, low);
}

static uint32_t patch_size(uint8_t code) {
    if (code == VITA_RELOC_ABS8) return 1u;
    if (code == VITA_RELOC_THM_JUMP11 ||
        code == VITA_RELOC_THM_JUMP8)
        return 2u;
    return 4u;
}

static int supported_code(uint8_t code) {
    switch (code) {
    case VITA_RELOC_NONE:
    case VITA_RELOC_ABS32:
    case VITA_RELOC_REL32:
    case VITA_RELOC_ABS8:
    case VITA_RELOC_THM_CALL:
    case VITA_RELOC_CALL:
    case VITA_RELOC_JUMP24:
    case VITA_RELOC_THM_JUMP24:
    case VITA_RELOC_TARGET1:
    case VITA_RELOC_V4BX:
    case VITA_RELOC_TARGET2:
    case VITA_RELOC_PREL31:
    case VITA_RELOC_MOVW_ABS_NC:
    case VITA_RELOC_MOVT_ABS:
    case VITA_RELOC_THM_MOVW_ABS_NC:
    case VITA_RELOC_THM_MOVT_ABS:
    case VITA_RELOC_THM_JUMP11:
    case VITA_RELOC_THM_JUMP8:
    case VITA_RELOC_RBASE:
        return 1;
    default:
        return 0;
    }
}

static void patch_entry(uint8_t *bytes, const vita_reloc_entry *entry) {
    switch (entry->code) {
    case VITA_RELOC_NONE:
    case VITA_RELOC_V4BX:
    case VITA_RELOC_RBASE:
        break;
    case VITA_RELOC_ABS32:
    case VITA_RELOC_TARGET1:
        wr32(bytes, entry->value);
        break;
    case VITA_RELOC_ABS8:
        bytes[0] = (uint8_t)entry->value;
        break;
    case VITA_RELOC_REL32:
    case VITA_RELOC_TARGET2:
        wr32(bytes, entry->value - entry->patch);
        break;
    case VITA_RELOC_PREL31:
        wr32(bytes, (entry->value - entry->patch) & 0x7FFFFFFFu);
        break;
    case VITA_RELOC_THM_CALL:
    case VITA_RELOC_THM_JUMP24:
        patch_thumb_call(bytes, entry->value - entry->patch);
        break;
    case VITA_RELOC_THM_JUMP11: {
        uint16_t half = rd16(bytes);
        wr16(bytes, (uint16_t)((half & 0xF800u) |
                              (((entry->value - entry->patch) >> 1) &
                               0x7FFu)));
        break;
    }
    case VITA_RELOC_THM_JUMP8: {
        uint16_t half = rd16(bytes);
        wr16(bytes, (uint16_t)((half & 0xFF00u) |
                              (((entry->value - entry->patch) >> 1) &
                               0xFFu)));
        break;
    }
    case VITA_RELOC_CALL:
    case VITA_RELOC_JUMP24: {
        uint32_t word = rd32(bytes);
        wr32(bytes, (word & 0xFF000000u) |
                    (((entry->value - entry->patch) >> 2) & 0xFFFFFFu));
        break;
    }
    case VITA_RELOC_MOVW_ABS_NC:
        patch_arm_mov(bytes, entry->value & 0xFFFFu);
        break;
    case VITA_RELOC_MOVT_ABS:
        patch_arm_mov(bytes, entry->value >> 16);
        break;
    case VITA_RELOC_THM_MOVW_ABS_NC:
        patch_thumb_mov(bytes, entry->value & 0xFFFFu);
        break;
    case VITA_RELOC_THM_MOVT_ABS:
        patch_thumb_mov(bytes, entry->value >> 16);
        break;
    default:
        break;
    }
}

static vita_reloc_status process_identity(
    const uint8_t *source,
    uint8_t *writable,
    size_t size,
    const vita_reloc_module *module,
    const vita_reloc_table *table,
    vita_reloc_identity *result) {
    uint32_t index;
    if (!source || !module || !table || !result ||
        (module->program_header_count && !module->program_headers))
        return VITA_RELOC_ERROR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    result->first_changed = VITA_RELOC_INDEX_NONE;
    result->first_unsupported = VITA_RELOC_INDEX_NONE;
    result->first_outside = VITA_RELOC_INDEX_NONE;
    for (index = 0; index < table->n; index++) {
        const vita_reloc_entry *entry = &table->v[index];
        seginfo patch_segment = seg_lookup(module, entry->patch_seg);
        uint32_t offset;
        uint32_t need;
        uint64_t file_offset;
        uint8_t expected[4];
        if (!supported_code(entry->code)) {
            if (!result->unsupported)
                result->first_unsupported = index;
            result->unsupported++;
            continue;
        }
        if (!patch_segment.ok) {
            if (!result->outside) result->first_outside = index;
            result->outside++;
            continue;
        }
        offset = entry->patch - patch_segment.base;
        need = patch_size(entry->code);
        file_offset = (uint64_t)patch_segment.file + offset;
        if (entry->patch < patch_segment.base ||
            (uint64_t)offset + need > patch_segment.filesz ||
            file_offset + need > size) {
            if (!result->outside) result->first_outside = index;
            result->outside++;
            continue;
        }
        memcpy(expected, source + file_offset, need);
        patch_entry(expected, entry);
        if (memcmp(source + file_offset, expected, need) != 0) {
            if (!result->changed) result->first_changed = index;
            result->changed++;
        }
        if (writable) memcpy(writable + file_offset, expected, need);
    }
    return VITA_RELOC_OK;
}

vita_reloc_status vita_reloc_verify_identity(
    const uint8_t *bytes,
    size_t size,
    const vita_reloc_module *module,
    const vita_reloc_table *table,
    vita_reloc_identity *result) {
    return process_identity(bytes, NULL, size, module, table, result);
}

vita_reloc_status vita_reloc_apply(
    uint8_t *bytes,
    size_t size,
    const vita_reloc_module *module,
    const vita_reloc_table *table,
    vita_reloc_identity *result) {
    return process_identity(bytes, bytes, size, module, table, result);
}

void vita_reloc_free(vita_reloc_table *table) {
    if (!table) return;
    free(table->v);
    memset(table, 0, sizeof(*table));
}

const char *vita_reloc_code_name(uint8_t code) {
    switch (code) {
    case VITA_RELOC_NONE: return "R_ARM_NONE";
    case VITA_RELOC_ABS32: return "R_ARM_ABS32";
    case VITA_RELOC_REL32: return "R_ARM_REL32";
    case VITA_RELOC_ABS8: return "R_ARM_ABS8";
    case VITA_RELOC_THM_CALL: return "R_ARM_THM_CALL";
    case VITA_RELOC_CALL: return "R_ARM_CALL";
    case VITA_RELOC_JUMP24: return "R_ARM_JUMP24";
    case VITA_RELOC_THM_JUMP24: return "R_ARM_THM_JUMP24";
    case VITA_RELOC_THM_JUMP11: return "R_ARM_THM_JUMP11";
    case VITA_RELOC_THM_JUMP8: return "R_ARM_THM_JUMP8";
    case VITA_RELOC_TARGET1: return "R_ARM_TARGET1";
    case VITA_RELOC_V4BX: return "R_ARM_V4BX";
    case VITA_RELOC_TARGET2: return "R_ARM_TARGET2";
    case VITA_RELOC_PREL31: return "R_ARM_PREL31";
    case VITA_RELOC_MOVW_ABS_NC: return "R_ARM_MOVW_ABS_NC";
    case VITA_RELOC_MOVT_ABS: return "R_ARM_MOVT_ABS";
    case VITA_RELOC_THM_MOVW_ABS_NC: return "R_ARM_THM_MOVW_ABS_NC";
    case VITA_RELOC_THM_MOVT_ABS: return "R_ARM_THM_MOVT_ABS";
    case VITA_RELOC_RBASE: return "R_ARM_RBASE";
    default: return "R_ARM_?";
    }
}
