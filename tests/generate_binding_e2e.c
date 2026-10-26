/* Build-time generator for executable cross-module binding coverage. */

#include "emit.h"
#include "nids.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A_BASE UINT32_C(0x10000000)
#define B_BASE UINT32_C(0x10001000)

#define CROSS_LIBRARY UINT32_C(0x11110000)
#define CROSS_NID     UINT32_C(0x11110001)
#define HLE_LIBRARY   UINT32_C(0x22220000)
#define HLE_NID       UINT32_C(0x22220001)
#define UNSUP_LIBRARY UINT32_C(0x33330000)
#define UNSUP_NID     UINT32_C(0x33330001)

#ifdef _WIN32
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif

static void put32(uint8_t *bytes, uint32_t offset, uint32_t value) {
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1u] = (uint8_t)(value >> 8u);
    bytes[offset + 2u] = (uint8_t)(value >> 16u);
    bytes[offset + 3u] = (uint8_t)(value >> 24u);
}

static uint32_t arm_bl(uint32_t source, uint32_t target) {
    int32_t displacement = (int32_t)(target - (source + 8u));
    return UINT32_C(0xEB000000) |
           ((uint32_t)(displacement >> 2) & UINT32_C(0x00FFFFFF));
}

static void add_function(vf_result *discovery, uint32_t address) {
    vf_func *function = &discovery->funcs[discovery->count++];
    memset(function, 0, sizeof(*function));
    function->addr = address;
    function->end = address + 8u;
    function->mode = ARM_A32;
    function->reasons = VF_R_EXPORT;
}

static int emit_one(const char *directory, const char *base,
                    const char *module_id, vm_image *image,
                    vm_module *module, const nid_db *db,
                    vf_result *discovery, int defaults) {
    char path[1024];
    FILE *output;
    emit_stats stats;
    if (snprintf(path, sizeof(path), "%s" PATH_SEPARATOR "%s.c",
                 directory, base) < 0)
        return 1;
    output = fopen(path, "wb");
    if (!output) return 1;
    if (em_emit_module(image, module, db, discovery, output, path,
                       defaults ? directory : NULL, module_id, 0u,
                       &stats) != 0) {
        fclose(output);
        return 1;
    }
    return fclose(output) != 0;
}

static int write_nids(const char *directory) {
    char path[1024];
    FILE *file;
    if (snprintf(path, sizeof(path),
                 "%s" PATH_SEPARATOR "binding.yml", directory) < 0)
        return 1;
    file = fopen(path, "wb");
    if (!file) return 1;
    fputs(
        "modules:\n"
        "  Fixture:\n"
        "    libraries:\n"
        "      FixtureCross:\n"
        "        nid: 0x11110000\n"
        "        functions:\n"
        "          crossModule: 0x11110001\n"
        "      FixtureHle:\n"
        "        nid: 0x22220000\n"
        "        functions:\n"
        "          hleOverride: 0x22220001\n"
        "      FixtureUnsupported:\n"
        "        nid: 0x33330000\n"
        "        functions:\n"
        "          namedUnsupported: 0x33330001\n",
        file);
    return fclose(file) != 0;
}

int main(int argc, char **argv) {
    uint8_t a_code[0x400] = {0};
    uint8_t b_code[0x400] = {0};
    vf_func a_functions[1];
    vf_func b_functions[4];
    vf_result a_discovery = {0};
    vf_result b_discovery = {0};
    vm_image a_image = {0};
    vm_image b_image = {0};
    vm_module a_module = {0};
    vm_module b_module = {0};
    vm_stub b_stubs[4] = {0};
    nid_db *db;
    int files = 0, entries = 0;
    int result;

    if (argc != 2 || write_nids(argv[1]) != 0) return 1;
    db = nid_db_load(argv[1], &files, &entries);
    if (!db || files != 1 || entries != 3) {
        nid_db_free(db);
        return 1;
    }
#ifdef _WIN32
    if (_putenv_s("VITARECOMP_EMIT_SPLIT", "0") != 0 ||
        _putenv_s("VITARECOMP_EMIT_PC", "1") != 0) {
        nid_db_free(db);
        return 1;
    }
#else
    if (setenv("VITARECOMP_EMIT_SPLIT", "0", 1) != 0 ||
        setenv("VITARECOMP_EMIT_PC", "1", 1) != 0) {
        nid_db_free(db);
        return 1;
    }
#endif

    put32(a_code, 0x00u, UINT32_C(0xE2800001)); /* ADD r0, r0, #1 */
    put32(a_code, 0x04u, UINT32_C(0xE12FFF1E)); /* BX lr */
    a_discovery.funcs = a_functions;
    a_discovery.cap = 1u;
    add_function(&a_discovery, A_BASE);
    a_image.data = a_code;
    a_image.size = sizeof(a_code);
    a_image.seg_vaddr = A_BASE;
    a_image.seg_len = sizeof(a_code);
    a_image.seg_mem_len = sizeof(a_code);
    memcpy(a_module.info.name, "binding_a", sizeof("binding_a"));
    a_module.info.module_start = UINT32_MAX;
    a_module.have_info = 1;
    a_module.metadata_complete = 1;

    put32(b_code, 0x00u, arm_bl(B_BASE + 0x00u, B_BASE + 0x80u));
    put32(b_code, 0x04u, UINT32_C(0xE12FFF1E));
    put32(b_code, 0x10u, arm_bl(B_BASE + 0x10u, B_BASE + 0x90u));
    put32(b_code, 0x14u, UINT32_C(0xE12FFF1E));
    put32(b_code, 0x20u, arm_bl(B_BASE + 0x20u, B_BASE + 0xA0u));
    put32(b_code, 0x24u, UINT32_C(0xE12FFF1E));
    put32(b_code, 0x30u, arm_bl(B_BASE + 0x30u, B_BASE + 0xB0u));
    put32(b_code, 0x34u, UINT32_C(0xE12FFF1E));
    b_discovery.funcs = b_functions;
    b_discovery.cap = 4u;
    add_function(&b_discovery, B_BASE + 0x00u);
    add_function(&b_discovery, B_BASE + 0x10u);
    add_function(&b_discovery, B_BASE + 0x20u);
    add_function(&b_discovery, B_BASE + 0x30u);
    b_image.data = b_code;
    b_image.size = sizeof(b_code);
    b_image.seg_vaddr = B_BASE;
    b_image.seg_len = sizeof(b_code);
    b_image.seg_mem_len = sizeof(b_code);
    memcpy(b_module.info.name, "binding_b", sizeof("binding_b"));
    b_module.info.module_start = UINT32_MAX;
    b_module.have_info = 1;
    b_module.metadata_complete = 1;
    b_module.stubs = b_stubs;
    b_module.stub_count = 4u;
    b_module.total_func_imports = 4u;
    b_stubs[0] = (vm_stub){
        B_BASE + 0x80u, B_BASE + 0x200u, CROSS_LIBRARY, CROSS_NID
    };
    b_stubs[1] = (vm_stub){
        B_BASE + 0x90u, B_BASE + 0x204u, CROSS_LIBRARY, CROSS_NID
    };
    b_stubs[2] = (vm_stub){
        B_BASE + 0xA0u, B_BASE + 0x208u, HLE_LIBRARY, HLE_NID
    };
    b_stubs[3] = (vm_stub){
        B_BASE + 0xB0u, B_BASE + 0x20Cu, UNSUP_LIBRARY, UNSUP_NID
    };

    result = emit_one(argv[1], "binding_a", "binding_a", &a_image,
                      &a_module, db, &a_discovery, 0) ||
             emit_one(argv[1], "binding_b", "binding_b", &b_image,
                      &b_module, db, &b_discovery, 1);
    nid_db_free(db);
    return result;
}
