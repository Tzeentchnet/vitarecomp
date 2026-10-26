/* Synthetic strict-loader validation and transactionality tests. */

#include "vitarecomp/loader.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/reloc.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

#define BASE UINT32_C(0x10000000)
#define MEMORY_SIZE UINT32_C(0x00010000)
#define IMAGE_SIZE 0x200u
#define PHOFF 0x34u
#define PH_SIZE 0x20u

static void wr16(uint8_t *bytes, size_t offset, uint16_t value) {
    bytes[offset + 0] = (uint8_t)value;
    bytes[offset + 1] = (uint8_t)(value >> 8);
}

static void wr32(uint8_t *bytes, size_t offset, uint32_t value) {
    bytes[offset + 0] = (uint8_t)value;
    bytes[offset + 1] = (uint8_t)(value >> 8);
    bytes[offset + 2] = (uint8_t)(value >> 16);
    bytes[offset + 3] = (uint8_t)(value >> 24);
}

static uint32_t rd32(const uint8_t *bytes, size_t offset) {
    return (uint32_t)bytes[offset + 0] |
           ((uint32_t)bytes[offset + 1] << 8) |
           ((uint32_t)bytes[offset + 2] << 16) |
           ((uint32_t)bytes[offset + 3] << 24);
}

static size_t ph_offset(uint32_t index) {
    return PHOFF + (size_t)index * PH_SIZE;
}

static void put_load(uint8_t *bytes, uint32_t index, uint32_t file_offset,
                     uint32_t address, uint32_t file_size,
                     uint32_t memory_size, uint32_t flags,
                     uint32_t alignment) {
    size_t ph = ph_offset(index);
    wr32(bytes, ph + 0x00, 1);
    wr32(bytes, ph + 0x04, file_offset);
    wr32(bytes, ph + 0x08, address);
    wr32(bytes, ph + 0x0C, address);
    wr32(bytes, ph + 0x10, file_size);
    wr32(bytes, ph + 0x14, memory_size);
    wr32(bytes, ph + 0x18, flags);
    wr32(bytes, ph + 0x1C, alignment);
}

static void put_rela(uint8_t *bytes, uint32_t index, uint32_t file_offset,
                     uint32_t file_size) {
    size_t ph = ph_offset(index);
    wr32(bytes, ph + 0x00, VITA_RELOC_PT_SCE_RELA);
    wr32(bytes, ph + 0x04, file_offset);
    wr32(bytes, ph + 0x10, file_size);
}

static void make_elf(uint8_t *bytes, uint32_t address_offset) {
    memset(bytes, 0, IMAGE_SIZE);
    bytes[0] = 0x7F;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 1;
    bytes[5] = 1;
    bytes[6] = 1;
    wr16(bytes, 0x10, 2);
    wr16(bytes, 0x12, 40);
    wr32(bytes, 0x14, 1);
    wr32(bytes, 0x18, BASE + address_offset + 0x101);
    wr32(bytes, 0x1C, PHOFF);
    wr16(bytes, 0x28, 0x34);
    wr16(bytes, 0x2A, PH_SIZE);
    wr16(bytes, 0x2C, 2);

    put_load(bytes, 0, 0x100, BASE + address_offset + 0x100,
             4, 8, 5, 0x10);
    put_load(bytes, 1, 0x110, BASE + address_offset + 0x200,
             3, 6, 6, 0x10);
    bytes[0x100] = 0x11;
    bytes[0x101] = 0x22;
    bytes[0x102] = 0x33;
    bytes[0x103] = 0x44;
    bytes[0x110] = 0xA1;
    bytes[0x111] = 0xB2;
    bytes[0x112] = 0xC3;
}

static vita_loader_module module_for(const uint8_t *bytes) {
    vita_loader_module module;
    memset(&module, 0, sizeof(module));
    module.bytes = bytes;
    module.size = IMAGE_SIZE;
    return module;
}

static void fill_guest(uint8_t value) {
    assert(vita_mem_set_guest(BASE, value, MEMORY_SIZE) == 0);
}

static void assert_guest_value(uint32_t address, uint8_t value,
                               size_t size) {
    uint8_t buffer[32];
    assert(size <= sizeof(buffer));
    assert(vita_mem_copy_from_guest(buffer, address, size) == 0);
    for (size_t i = 0; i < size; i++) assert(buffer[i] == value);
}

static void expect_failure(const uint8_t *bytes, size_t size,
                           vita_loader_status expected,
                           uint32_t program_header) {
    vita_loader_module module = module_for(bytes);
    module.size = size;
    vita_loader_error error;
    assert(vita_preflight_module(&module, NULL, &error) == expected);
    assert(error.status == expected);
    assert(error.module_index == 0);
    assert(error.program_header_index == program_header);
    assert(strcmp(vita_loader_status_string(expected), "unknown loader status")
           != 0);
}

static void test_valid_load_and_metadata(void) {
    uint8_t image[IMAGE_SIZE];
    make_elf(image, 0);

    vita_loader_segment_metadata segments[2] = {
        {0, 0x100, BASE + 0x100, BASE + 0x100, 4, 8, 5, 0x10},
        {1, 0x110, BASE + 0x200, BASE + 0x200, 3, 6, 6, 0x10},
    };
    vita_loader_module_metadata expected = {
        2, 40, BASE + 0x101, 0, 2, 0, 2, segments
    };
    vita_loader_module module = module_for(image);
    module.expected = &expected;
    vita_load_info info = {0};
    vita_loader_error error;

    fill_guest(0xCC);
    assert(vita_preflight_module(&module, &info, &error) == VITA_LOADER_OK);
    assert(error.status == VITA_LOADER_OK);
    assert(info.lowest == BASE + 0x100);
    assert(info.highest == BASE + 0x206);
    assert(info.segments == 2);
    assert_guest_value(BASE + 0x100, 0xCC, 8);

    assert(vita_load_module(&module, &info, &error) == VITA_LOADER_OK);
    {
        const uint8_t expected_text[8] =
            {0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0};
        const uint8_t expected_data[6] =
            {0xA1, 0xB2, 0xC3, 0, 0, 0};
        uint8_t actual[8];
        assert(vita_mem_copy_from_guest(actual, BASE + 0x100, 8) == 0);
        assert(memcmp(actual, expected_text, 8) == 0);
        assert(vita_mem_copy_from_guest(actual, BASE + 0x200, 6) == 0);
        assert(memcmp(actual, expected_data, 6) == 0);
    }

    fill_guest(0x5A);
    segments[0].file_size++;
    assert(vita_load_module(&module, NULL, &error) ==
           VITA_LOADER_ERROR_METADATA_MISMATCH);
    assert(error.program_header_index == 0);
    assert(error.record_index == 0);
    assert_guest_value(BASE + 0x100, 0x5A, 8);
    puts("  strict load, BSS, metadata       ok");
}

static void test_malformed_headers_and_segments(void) {
    uint8_t image[IMAGE_SIZE];

    make_elf(image, 0);
    expect_failure(image, 16, VITA_LOADER_ERROR_TRUNCATED,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    image[0] = 0;
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_BAD_MAGIC,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    image[4] = 2;
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_UNSUPPORTED_CLASS,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    image[5] = 2;
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_UNSUPPORTED_ENDIAN,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    image[6] = 0;
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_UNSUPPORTED_VERSION,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    wr16(image, 0x10, 3);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_UNSUPPORTED_TYPE,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    wr16(image, 0x12, 3);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_UNSUPPORTED_MACHINE,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    wr16(image, 0x28, 0x33);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_HEADER_SIZE,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    wr16(image, 0x2A, 0x1C);
    expect_failure(image, IMAGE_SIZE,
                   VITA_LOADER_ERROR_PROGRAM_HEADER_SIZE,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    wr32(image, 0x1C, UINT32_MAX);
    expect_failure(image, IMAGE_SIZE,
                   VITA_LOADER_ERROR_PROGRAM_HEADER_TABLE,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    wr32(image, ph_offset(1) + 0x00, 4);
    wr32(image, ph_offset(1) + 0x04, IMAGE_SIZE - 2);
    wr32(image, ph_offset(1) + 0x10, 4);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_FILE_RANGE, 1);

    make_elf(image, 0);
    wr32(image, ph_offset(0) + 0x04, UINT32_MAX - 1u);
    wr32(image, ph_offset(0) + 0x10, 4);
    expect_failure(image, IMAGE_SIZE,
                   VITA_LOADER_ERROR_FILE_RANGE_OVERFLOW, 0);

    make_elf(image, 0);
    wr32(image, ph_offset(0) + 0x10, 9);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_FILE_SIZE, 0);

    make_elf(image, 0);
    wr32(image, ph_offset(0) + 0x08, UINT32_MAX - 3u);
    wr32(image, ph_offset(0) + 0x14, 8);
    wr32(image, ph_offset(0) + 0x1C, 1);
    expect_failure(image, IMAGE_SIZE,
                   VITA_LOADER_ERROR_VIRTUAL_RANGE_OVERFLOW, 0);

    make_elf(image, 0);
    wr32(image, ph_offset(0) + 0x10, 0);
    wr32(image, ph_offset(0) + 0x14, 0);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_ZERO_LOAD_RANGE, 0);

    make_elf(image, 0);
    wr32(image, ph_offset(1) + 0x08, BASE + 0x104);
    wr32(image, ph_offset(1) + 0x1C, 1);
    expect_failure(image, IMAGE_SIZE,
                   VITA_LOADER_ERROR_LOAD_RANGE_OVERLAP, 1);

    make_elf(image, 0);
    wr32(image, ph_offset(0) + 0x18, 13);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_LOAD_FLAGS, 0);

    make_elf(image, 0);
    wr32(image, ph_offset(0) + 0x18, 7);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_LOAD_FLAGS, 0);

    make_elf(image, 0);
    wr32(image, ph_offset(0) + 0x1C, 3);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_ALIGNMENT, 0);

    make_elf(image, 0);
    wr32(image, ph_offset(0) + 0x08, BASE + 0x101);
    wr32(image, ph_offset(0) + 0x1C, 0x100);
    expect_failure(image, IMAGE_SIZE, VITA_LOADER_ERROR_ALIGNMENT, 0);

    make_elf(image, 0);
    wr32(image, ph_offset(0) + 0x00, 4);
    wr32(image, ph_offset(1) + 0x00, 4);
    expect_failure(image, IMAGE_SIZE,
                   VITA_LOADER_ERROR_NO_LOAD_SEGMENTS,
                   VITA_LOADER_INDEX_NONE);

    make_elf(image, 0);
    wr32(image, ph_offset(1) + 0x00, 2);
    expect_failure(image, IMAGE_SIZE,
                   VITA_LOADER_ERROR_UNSUPPORTED_LAYOUT, 1);

    puts("  malformed ELF and PT_LOADs       ok");
}

static void test_module_set_transactionality(void) {
    uint8_t first[IMAGE_SIZE];
    uint8_t second[IMAGE_SIZE];
    make_elf(first, 0);
    make_elf(second, 0x1000);
    vita_loader_module modules[2] = {
        module_for(first), module_for(second)
    };
    vita_loader_error error;

    fill_guest(0x7D);
    wr32(second, ph_offset(0) + 0x10, 9);
    assert(vita_load_module_set(modules, 2, NULL, &error) ==
           VITA_LOADER_ERROR_FILE_SIZE);
    assert(error.module_index == 1);
    assert(error.program_header_index == 0);
    assert_guest_value(BASE + 0x100, 0x7D, 8);

    make_elf(second, 0);
    fill_guest(0x6C);
    assert(vita_load_module_set(modules, 2, NULL, &error) ==
           VITA_LOADER_ERROR_CROSS_MODULE_OVERLAP);
    assert(error.module_index == 1);
    assert(error.related_module_index == 0);
    assert_guest_value(BASE + 0x100, 0x6C, 8);

    make_elf(second, 0x1000);
    fill_guest(0x4B);
    vita_load_info infos[2];
    assert(vita_load_module_set(modules, 2, infos, &error) ==
           VITA_LOADER_OK);
    assert(infos[0].segments == 2 && infos[1].segments == 2);
    assert(vita_read8(BASE + 0x100) == 0x11);
    assert(vita_read8(BASE + 0x1100) == 0x11);

    make_elf(second, MEMORY_SIZE);
    fill_guest(0x39);
    assert(vita_load_module_set(modules, 2, NULL, &error) ==
           VITA_LOADER_ERROR_GUEST_RANGE);
    assert(error.module_index == 1);
    assert_guest_value(BASE + 0x100, 0x39, 8);
    puts("  module-set transactionality      ok");
}

static void make_relocation_elf(uint8_t *image, uint32_t stream_size) {
    make_elf(image, 0);
    wr16(image, 0x10, 0xFE04);
    wr16(image, 0x2C, 3);
    put_rela(image, 2, 0x180, stream_size);
}

static vita_reloc_status validate_relocations(
    uint8_t *image,
    vita_reloc_table *table,
    vita_reloc_identity *identity,
    vita_reloc_error *error) {
    return vita_reloc_validate_elf_identity(
        image, IMAGE_SIZE, table, identity, error);
}

static void test_relocation_identity_validation(void) {
    uint8_t image[IMAGE_SIZE];
    uint8_t original[IMAGE_SIZE];
    vita_reloc_table table;
    vita_reloc_identity identity;
    vita_reloc_error error;

    make_relocation_elf(image, 12);
    wr32(image, 0x180, (0xFu << 4) | (VITA_RELOC_ABS32 << 8));
    wr32(image, 0x184, 0x44332211u);
    wr32(image, 0x188, 0);
    memcpy(original, image, sizeof(image));
    assert(validate_relocations(image, &table, &identity, &error) ==
           VITA_RELOC_OK);
    assert(table.streams == 1 && table.n == 1);
    assert(identity.changed == 0 && identity.unsupported == 0 &&
           identity.outside == 0);
    assert(memcmp(image, original, sizeof(image)) == 0);
    vita_reloc_free(&table);

    make_relocation_elf(image, 1);
    image[0x180] = 0;
    assert(validate_relocations(image, &table, &identity, &error) ==
           VITA_RELOC_ERROR_TRUNCATED_RECORD);
    assert(table.trailing == 1);
    vita_reloc_free(&table);

    make_relocation_elf(image, 4);
    wr32(image, 0x180, 0xAu);
    assert(validate_relocations(image, &table, &identity, &error) ==
           VITA_RELOC_ERROR_UNKNOWN_FORMAT);
    vita_reloc_free(&table);

    make_relocation_elf(image, 13);
    wr32(image, 0x180, (0xFu << 4) | (VITA_RELOC_ABS32 << 8));
    wr32(image, 0x184, 0x44332211u);
    wr32(image, 0x188, 0);
    image[0x18C] = 0;
    assert(validate_relocations(image, &table, &identity, &error) ==
           VITA_RELOC_ERROR_TRUNCATED_RECORD);
    assert(table.n == 1 && table.trailing == 1);
    vita_reloc_free(&table);

    make_relocation_elf(image, 12);
    wr32(image, 0x180, (0xFu << 4) | (0x7Eu << 8));
    wr32(image, 0x184, 0);
    wr32(image, 0x188, 0);
    assert(validate_relocations(image, &table, &identity, &error) ==
           VITA_RELOC_ERROR_UNSUPPORTED_CODE);
    assert(identity.unsupported == 1);
    vita_reloc_free(&table);

    make_relocation_elf(image, 12);
    wr32(image, 0x180,
         (0xFu << 4) | (VITA_RELOC_ABS32 << 8) | (3u << 16));
    wr32(image, 0x184, 0x44332211u);
    wr32(image, 0x188, 0);
    assert(validate_relocations(image, &table, &identity, &error) ==
           VITA_RELOC_ERROR_BAD_SEGMENT);
    vita_reloc_free(&table);

    make_relocation_elf(image, 12);
    wr32(image, 0x180, (0xFu << 4) | (VITA_RELOC_ABS32 << 8));
    wr32(image, 0x184, 0x44332211u);
    wr32(image, 0x188, 8);
    assert(validate_relocations(image, &table, &identity, &error) ==
           VITA_RELOC_ERROR_PATCH_OUTSIDE_FILE);
    assert(identity.outside == 1);
    vita_reloc_free(&table);

    make_relocation_elf(image, 12);
    wr32(image, 0x180, (0xFu << 4) | (VITA_RELOC_ABS32 << 8));
    wr32(image, 0x184, 0x88776655u);
    wr32(image, 0x188, 0);
    memcpy(original, image, sizeof(image));
    assert(validate_relocations(image, &table, &identity, &error) ==
           VITA_RELOC_ERROR_NOT_IDENTITY);
    assert(identity.changed == 1);
    assert(memcmp(image, original, sizeof(image)) == 0);
    vita_reloc_free(&table);

    make_elf(image, 0);
    wr16(image, 0x10, 0xFE00);
    assert(validate_relocations(image, &table, &identity, &error) ==
           VITA_RELOC_OK);
    assert(table.streams == 0 && table.n == 0);
    vita_reloc_free(&table);
    puts("  strict relocation identity       ok");
}

static void test_relocation_identity_normalization(void) {
    uint8_t image[IMAGE_SIZE];
    uint8_t original[IMAGE_SIZE];
    vita_reloc_table table;
    vita_reloc_identity identity;
    vita_reloc_error error;

    make_relocation_elf(image, 12);
    wr32(image, 0x180, (0xFu << 4) | (VITA_RELOC_ABS32 << 8));
    wr32(image, 0x184, 0x88776655u);
    wr32(image, 0x188, 0);
    memcpy(original, image, sizeof(image));
    assert(vita_reloc_normalize_elf_identity(
               image, sizeof(image), &table, &identity, &error) ==
           VITA_RELOC_OK);
    assert(identity.changed == 1 && identity.unsupported == 0 &&
           identity.outside == 0);
    assert(rd32(image, 0x100) == 0x88776655u);
    assert(memcmp(image, original, sizeof(image)) != 0);
    vita_reloc_free(&table);

    assert(vita_reloc_normalize_elf_identity(
               image, sizeof(image), &table, &identity, &error) ==
           VITA_RELOC_OK);
    assert(identity.changed == 0 && identity.unsupported == 0 &&
           identity.outside == 0);
    vita_reloc_free(&table);

    make_relocation_elf(image, 12);
    wr32(image, 0x180, (0xFu << 4) | (VITA_RELOC_ABS32 << 8));
    wr32(image, 0x184, 0x88776655u);
    wr32(image, 0x188, 0);
    memcpy(original, image, sizeof(image));
    {
        const uint32_t post_link_patch = BASE + 0x100u;
        assert(vita_reloc_normalize_elf_identity_except(
                   image, sizeof(image), &post_link_patch, 1u,
                   &table, &identity, &error) == VITA_RELOC_OK);
    }
    assert(identity.changed == 0 && identity.preserved == 1 &&
           identity.unsupported == 0 && identity.outside == 0);
    assert(rd32(image, 0x100) == 0x44332211u);
    assert(memcmp(image, original, sizeof(image)) == 0);
    vita_reloc_free(&table);

    make_relocation_elf(image, 12);
    wr32(image, 0x180, (0xFu << 4) | (0x7Eu << 8));
    wr32(image, 0x184, 0);
    wr32(image, 0x188, 0);
    memcpy(original, image, sizeof(image));
    assert(vita_reloc_normalize_elf_identity(
               image, sizeof(image), &table, &identity, &error) ==
           VITA_RELOC_ERROR_UNSUPPORTED_CODE);
    assert(memcmp(image, original, sizeof(image)) == 0);
    vita_reloc_free(&table);

    make_relocation_elf(image, 12);
    wr32(image, 0x180, (0xFu << 4) | (VITA_RELOC_ABS32 << 8));
    wr32(image, 0x184, 0x88776655u);
    wr32(image, 0x188, 8);
    memcpy(original, image, sizeof(image));
    assert(vita_reloc_normalize_elf_identity(
               image, sizeof(image), &table, &identity, &error) ==
           VITA_RELOC_ERROR_PATCH_OUTSIDE_FILE);
    assert(memcmp(image, original, sizeof(image)) == 0);
    vita_reloc_free(&table);

    make_relocation_elf(image, 1);
    image[0x180] = 0;
    memcpy(original, image, sizeof(image));
    assert(vita_reloc_normalize_elf_identity(
               image, sizeof(image), &table, &identity, &error) ==
           VITA_RELOC_ERROR_TRUNCATED_RECORD);
    assert(memcmp(image, original, sizeof(image)) == 0);
    vita_reloc_free(&table);

    make_elf(image, 0);
    wr16(image, 0x10, 0xFE00);
    memcpy(original, image, sizeof(image));
    assert(vita_reloc_normalize_elf_identity(
               image, sizeof(image), &table, &identity, &error) ==
           VITA_RELOC_OK);
    assert(identity.changed == 0 && table.n == 0 && table.streams == 0);
    assert(memcmp(image, original, sizeof(image)) == 0);
    vita_reloc_free(&table);
    puts("  relocation normalization        ok");
}

static void test_variable_relocation_blocks(void) {
    uint8_t block[32];
    vita_var_reloc_table table;
    vita_reloc_error error;

    memset(block, 0, sizeof(block));
    wr32(block, 0, 24u << 4);
    wr32(block, 4, 1u | (2u << 4) |
                       (VITA_RELOC_ABS32 << 8) | (0xABCDu << 16));
    wr32(block, 8, 0x10203040u);
    wr32(block, 12, 2u | (3u << 4) |
                        (VITA_RELOC_THM_MOVW_ABS_NC << 8));
    wr32(block, 16, 0x50607080u);
    wr32(block, 20, 0x89ABCDEFu);
    assert(vita_var_reloc_parse_block(
               block, sizeof(block), &table, &error) == VITA_RELOC_OK);
    assert(table.declared_bytes == 24u && table.n == 2u);
    assert(table.by_format[1] == 1u && table.by_format[2] == 1u);
    assert(table.v[0].format == 1u && table.v[0].patch_seg == 2u &&
           table.v[0].code == VITA_RELOC_ABS32 &&
           table.v[0].offset == 0x10203040u &&
           table.v[0].addend == 0xABCDu);
    assert(table.v[1].format == 2u && table.v[1].patch_seg == 3u &&
           table.v[1].code == VITA_RELOC_THM_MOVW_ABS_NC &&
           table.v[1].offset == 0x50607080u &&
           table.v[1].addend == 0x89ABCDEFu);
    vita_var_reloc_free(&table);

    wr32(block, 0, 12u << 4);
    wr32(block, 4, 2u);
    assert(vita_var_reloc_parse_block(
               block, 12u, &table, &error) ==
           VITA_RELOC_ERROR_TRUNCATED_RECORD);
    vita_var_reloc_free(&table);

    wr32(block, 0, 8u << 4);
    wr32(block, 4, 3u);
    assert(vita_var_reloc_parse_block(
               block, 8u, &table, &error) ==
           VITA_RELOC_ERROR_UNKNOWN_FORMAT);
    vita_var_reloc_free(&table);
    puts("  variable relocation blocks      ok");
}

typedef struct extension_state {
    uint32_t calls;
    uint32_t requested;
    int mode;
} extension_state;

static vita_loader_status validate_extension(
    const uint8_t *bytes,
    size_t size,
    const vita_loader_module_metadata *metadata,
    uint32_t requested_validation,
    uint32_t *completed_validation,
    vita_loader_error *error,
    void *userdata) {
    extension_state *state = (extension_state *)userdata;
    assert(bytes && size == IMAGE_SIZE);
    assert(metadata->segment_count == 2);
    state->calls++;
    state->requested = requested_validation;
    if (state->mode == 1) {
        *completed_validation = VITA_LOADER_VALIDATE_MODULE_INFO;
        return VITA_LOADER_OK;
    }
    if (state->mode == 2) {
        error->program_header_index = 1;
        error->record_index = 7;
        return VITA_LOADER_ERROR_RELOCATION;
    }
    *completed_validation = requested_validation;
    return VITA_LOADER_OK;
}

static void test_validation_extensions(void) {
    uint8_t image[IMAGE_SIZE];
    make_elf(image, 0x2000);
    wr16(image, 0x10, 0xFE04);
    vita_loader_module module = module_for(image);
    vita_loader_error error;

    assert(vita_preflight_module(&module, NULL, &error) ==
           VITA_LOADER_ERROR_VALIDATION_REQUIRED);
    assert(error.required_validation ==
           (VITA_LOADER_VALIDATE_MODULE_INFO |
            VITA_LOADER_VALIDATE_RELOCATIONS));

    extension_state state = {0};
    module.validate = validate_extension;
    module.validation_userdata = &state;
    wr32(image, ph_offset(0) + 0x10, 9);
    assert(vita_preflight_module(&module, NULL, &error) ==
           VITA_LOADER_ERROR_FILE_SIZE);
    assert(state.calls == 0);
    wr32(image, ph_offset(0) + 0x10, 4);
    state.mode = 1;
    assert(vita_preflight_module(&module, NULL, &error) ==
           VITA_LOADER_ERROR_VALIDATION_REQUIRED);
    assert(error.completed_validation == VITA_LOADER_VALIDATE_MODULE_INFO);

    state.mode = 2;
    assert(vita_preflight_module(&module, NULL, &error) ==
           VITA_LOADER_ERROR_RELOCATION);
    assert(error.module_index == 0);
    assert(error.program_header_index == 1);
    assert(error.record_index == 7);

    state.mode = 0;
    assert(vita_preflight_module(&module, NULL, &error) == VITA_LOADER_OK);
    assert(state.requested ==
           (VITA_LOADER_VALIDATE_MODULE_INFO |
            VITA_LOADER_VALIDATE_RELOCATIONS));
    assert(state.calls == 3);
    puts("  strict validation extensions     ok");
}

int main(void) {
    assert(vita_mem_init(BASE, MEMORY_SIZE) == 0);
    printf("loader:\n");
    test_valid_load_and_metadata();
    test_malformed_headers_and_segments();
    test_module_set_transactionality();
    test_relocation_identity_validation();
    test_relocation_identity_normalization();
    test_variable_relocation_blocks();
    test_validation_extensions();
    vita_mem_free();
    {
        uint8_t image[IMAGE_SIZE];
        vita_load_info info;
        vita_loader_error error;
        make_elf(image, 0);
        vita_loader_module module = module_for(image);
        assert(vita_preflight_module(&module, &info, &error) ==
               VITA_LOADER_OK);
        assert(info.lowest == BASE + 0x100 && info.segments == 2);
    }
    puts("  preflight without guest memory   ok");
    puts("all loader tests passed");
    return 0;
}
