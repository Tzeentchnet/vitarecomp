/* Synthetic emitter regressions for prior AOT semantic vectors. */

#include "emit.h"
#include "compose.h"
#include "vitarecomp/generated_module.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

#define BASE 0x1000u

_Static_assert(VITA_GENERATED_MODULE_ABI_VERSION == 4u,
               "generated-module ABI version changed unexpectedly");
_Static_assert(VITA_GENERATED_REPORT_SCHEMA_VERSION == 4u,
               "generated report schema changed unexpectedly");

static void put16(uint8_t *code, uint32_t off, uint16_t value) {
    code[off] = (uint8_t)value;
    code[off + 1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *code, uint32_t off, uint32_t value) {
    code[off] = (uint8_t)value;
    code[off + 1] = (uint8_t)(value >> 8);
    code[off + 2] = (uint8_t)(value >> 16);
    code[off + 3] = (uint8_t)(value >> 24);
}

static void add_func(vf_result *disc, uint32_t addr, arm_mode mode) {
    vf_func *fn = &disc->funcs[disc->count++];
    memset(fn, 0, sizeof(*fn));
    fn->addr = addr;
    fn->mode = mode;
    fn->end = addr + 8;
    fn->reasons = VF_R_EXPORT;
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    assert(n >= 0);
    rewind(f);
    char *text = (char *)malloc((size_t)n + 1);
    assert(text);
    assert(fread(text, 1, (size_t)n, f) == (size_t)n);
    text[n] = '\0';
    fclose(f);
    return text;
}

static unsigned count_text(const char *text, const char *needle) {
    unsigned count = 0;
    size_t n = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += n;
    }
    return count;
}

static void test_module_metadata_failures(void) {
    uint8_t bytes[0x200];
    vm_image img;
    vm_module mod;
    char err[128];

    memset(bytes, 0, sizeof(bytes));
    memset(&img, 0, sizeof(img));
    img.data = bytes;
    img.size = sizeof(bytes);
    img.seg_vaddr = BASE;
    img.seg_len = sizeof(bytes);
    put16(bytes, 0x02, 0x0101);
    put32(bytes, 0x44, 0xFFFFFFFFu);

    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) == 0);
    assert(mod.metadata_complete == 1);
    vm_free(&mod);

    put32(bytes, 0x24, 0x80);
    put32(bytes, 0x28, 0xA0);
    put16(bytes, 0x80, 0x20);
    put16(bytes, 0x86, 1);
    put16(bytes, 0x88, 1);
    put16(bytes, 0x8A, 1);
    put32(bytes, 0x8C, 0x11223344u);
    put32(bytes, 0x98, BASE + 0xC0);
    put32(bytes, 0x9C, BASE + 0xD0);
    put32(bytes, 0xC0, 0x01020304u);
    put32(bytes, 0xC4, 0x05060708u);
    put32(bytes, 0xC8, 0x090A0B0Cu);
    put32(bytes, 0xD0, BASE + 0x101);
    put32(bytes, 0xD4, BASE + 0x120);
    put32(bytes, 0xD8, BASE + 0x130);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) == 0);
    assert(mod.export_count == 3);
    assert(mod.function_export_count == 1);
    assert(mod.variable_export_count == 1);
    assert(mod.tls_variable_export_count == 1);
    assert(mod.exports[0].address == BASE + 0x101);
    vm_free(&mod);

    img.data_seg_count = 1;
    img.data_segs[0].vaddr = BASE + 0x200;
    img.data_segs[0].len = 0;
    img.data_segs[0].mem_len = 0x20;
    put32(bytes, 0xD0, BASE + 0x201);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) == 0);
    assert(mod.function_export_count == 0);
    assert(mod.variable_export_count == 2);
    assert(mod.exports[0].kind == VM_EXPORT_VARIABLE);
    assert(mod.exports[0].address == BASE + 0x201);
    vm_free(&mod);

    put32(bytes, 0xD0, BASE + 0x301);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) != 0);
    assert(strstr(err, "exported function"));
    put32(bytes, 0xD0, BASE + 0x101);
    img.data_seg_count = 0;

    put32(bytes, 0x24, 0x80);
    put32(bytes, 0x28, 0x84);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) != 0);
    assert(strstr(err, "export"));

    put32(bytes, 0x24, 0);
    put32(bytes, 0x28, 0);
    put32(bytes, 0x2C, 0x80);
    put32(bytes, 0x30, 0x84);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) != 0);
    assert(strstr(err, "import"));

    put32(bytes, 0x30, 0xA4);
    put16(bytes, 0x80, 0x24);
    put16(bytes, 0x86, 2);
    put16(bytes, 0x88, 0);
    put16(bytes, 0x8A, 0);
    put32(bytes, 0x8C, 0x11223344u);
    put32(bytes, 0x94, BASE + 0xC0);
    put32(bytes, 0x98, BASE + 0xC8);
    put32(bytes, 0xC0, 0xAABBCCDDu);
    put32(bytes, 0xC4, 0xAABBCCDDu);
    put32(bytes, 0xC8, BASE + 0x151);
    put32(bytes, 0xCC, BASE + 0x140);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) == 0);
    assert(mod.stub_count == 2);
    assert(mod.stubs[0].addr == BASE + 0x140);
    assert(mod.stubs[0].dispatch_key == BASE + 0x140);
    assert(mod.stubs[0].target_site == BASE + 0xCC);
    assert(mod.stubs[1].addr == BASE + 0x150);
    assert(mod.stubs[1].dispatch_key == BASE + 0x151);
    assert(mod.stubs[1].target_site == BASE + 0xC8);
    assert(mod.stubs[0].func_nid == mod.stubs[1].func_nid);
    vm_free(&mod);

    put16(bytes, 0x8A, 1);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) != 0);
    assert(strstr(err, "TLS variable imports"));
}

static void test_variable_relocation_segments(void) {
    uint8_t bytes[0x240];
    vm_image img;
    vm_module mod;
    char err[128];

    memset(bytes, 0, sizeof(bytes));
    memset(&img, 0, sizeof(img));
    img.data = bytes;
    img.size = sizeof(bytes);
    img.seg_vaddr = BASE;
    img.seg_len = 0x1E0;
    img.seg_program_header_index = 0;
    img.data_seg_count = 1;
    img.data_segs[0].file = 0x1E0;
    img.data_segs[0].vaddr = 0x3000;
    img.data_segs[0].len = 8;
    img.data_segs[0].mem_len = 8;
    img.data_segs[0].program_header_index = 2;

    put16(bytes, 0x02, 0x0101);
    put32(bytes, 0x2C, 0x80);
    put32(bytes, 0x30, 0xA4);
    put32(bytes, 0x44, 0xFFFFFFFFu);
    put16(bytes, 0x80, 0x24);
    put16(bytes, 0x88, 1);
    put32(bytes, 0x8C, 0x11223344u);
    put32(bytes, 0x9C, BASE + 0xC0);
    put32(bytes, 0xA0, BASE + 0xC4);
    put32(bytes, 0xC0, 0x55667788u);
    put32(bytes, 0xC4, BASE + 0xD0);
    put32(bytes, 0xD0, 24u << 4);
    put32(bytes, 0xD4,
          1u | (2u << 4) | (2u << 8) | (0xABCDu << 16));
    put32(bytes, 0xD8, 0);
    put32(bytes, 0xDC, 2u | (2u << 4) | (2u << 8));
    put32(bytes, 0xE0, 4);
    put32(bytes, 0xE4, 0x89ABCDEFu);

    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) == 0);
    assert(mod.var_count == 1 && mod.vars[0].site_count == 2);
    assert(mod.vars[0].sites[0].addr == 0x3000);
    assert(mod.vars[0].sites[0].addend == 0xABCDu);
    assert(mod.vars[0].sites[0].code == 2);
    assert(mod.vars[0].sites[1].addr == 0x3004);
    assert(mod.vars[0].sites[1].addend == 0x89ABCDEFu);
    assert(mod.vars[0].sites[1].code == 2);
    vm_free(&mod);
}

int main(void) {
    static uint8_t code[0x100];
    static vf_func funcs[10];
    vm_image img;
    vm_module mod;
    vf_result disc;
    emit_stats stats;

    memset(&img, 0, sizeof(img));
    memset(&mod, 0, sizeof(mod));
    memset(&disc, 0, sizeof(disc));
    memset(&stats, 0, sizeof(stats));
    assert(em_module_id_valid("synthetic_mod"));
    assert(em_module_id_valid("Module2"));
    assert(!em_module_id_valid(NULL));
    assert(!em_module_id_valid(""));
    assert(!em_module_id_valid("2module"));
    assert(!em_module_id_valid("module-name"));
    char long_module_id[EM_MODULE_ID_MAX + 2];
    memset(long_module_id, 'a', sizeof(long_module_id) - 1);
    long_module_id[sizeof(long_module_id) - 1] = '\0';
    assert(!em_module_id_valid(long_module_id));
    test_module_metadata_failures();
    test_variable_relocation_segments();
    disc.funcs = funcs;
    disc.cap = (uint32_t)(sizeof(funcs) / sizeof(funcs[0]));
    img.data = code;
    img.size = sizeof(code);
    img.seg_vaddr = BASE;
    img.seg_len = sizeof(code);
    img.seg_align = 64u;
    memcpy(mod.info.name, "synthetic", 10);

    /* Exact prior vectors, each followed by an unconditional return. */
    put32(code, 0x00, 0x1B000002u); /* BLNE 0x1010 */
    put32(code, 0x04, 0xE12FFF1Eu); /* BX LR */
    put32(code, 0x10, 0xE12FFF1Eu);
    put32(code, 0x20, 0xE12FFF3Eu); /* BLX LR */
    put32(code, 0x24, 0xE12FFF1Eu);
    put32(code, 0x30, 0x112FFF1Eu); /* BXNE LR */
    put32(code, 0x34, 0xE12FFF1Eu);
    put32(code, 0x40, 0x112FFF13u); /* BXNE r3 */
    put32(code, 0x44, 0xE12FFF1Eu);
    put32(code, 0x50, 0x1F000007u); /* SVCNE #7 */
    put32(code, 0x54, 0xE12FFF1Eu);
    put32(code, 0x60, 0xE12FFF1Eu); /* Same address is also seeded as Thumb. */
    put16(code, 0x70, 0xF980u);     /* VST1.8 {d0[0]}, [r0]! */
    put16(code, 0x72, 0x000Du);
    put16(code, 0x74, 0x4770u);
    put32(code, 0x80, 0xEA00005Eu); /* B 0x1200, outside the segment. */
    put32(code, 0x84, 0xE12FFF1Eu);

    add_func(&disc, BASE + 0x80, ARM_A32);
    add_func(&disc, BASE + 0x70, ARM_T32);
    add_func(&disc, BASE + 0x60, ARM_T32);
    add_func(&disc, BASE + 0x60, ARM_A32);
    add_func(&disc, BASE + 0x50, ARM_A32);
    add_func(&disc, BASE + 0x40, ARM_A32);
    add_func(&disc, BASE + 0x30, ARM_A32);
    add_func(&disc, BASE + 0x20, ARM_A32);
    add_func(&disc, BASE + 0x10, ARM_A32);
    add_func(&disc, BASE + 0x00, ARM_A32);

#ifdef _WIN32
    assert(_putenv_s("VITARECOMP_EMIT_SPLIT", "0") == 0);
    assert(_putenv_s("VITARECOMP_EMIT_PC", "0") == 0);
#else
    assert(setenv("VITARECOMP_EMIT_SPLIT", "0", 1) == 0);
    assert(setenv("VITARECOMP_EMIT_PC", "0", 1) == 0);
#endif

    const char *outpath = "test_emit_generated.c";
    FILE *out = fopen(outpath, "wb");
    assert(out);
    assert(em_emit(&img, &mod, NULL, &disc, out, outpath, ".", 0, &stats) == 0);
    fclose(out);

    char *text = read_all(outpath);
    assert(strstr(text, "void vita_func_00001060(void)"));
    assert(strstr(text, "void vita_func_00001061(void)"));
    assert(count_text(text, "if (vita_cond(1)) {") >= 4);
    assert(strstr(text,
        "{ uint32_t vita_blx_target = lr; lr = 0x00001024u; "
        "vita_dispatch(0x00001020, vita_blx_target); }"));
    assert(strstr(text, "vita_dispatch(0x00001040, r3); return;"));
    assert(strstr(text, "vita_trap_unimpl(0x00001050, 0x1F000007"));
    assert(strstr(text,
        "vita_write8(r0 + 0, (uint8_t)vita_neon_lane(vita_d_get(0), 0, 8));"));
    assert(!strstr(text, "vita_write16(r0 + 0"));
    assert(!strstr(text, "vita_write32(r0 + 0"));
    assert(stats.trapped >= 1);
    uint32_t classified_traps = stats.traps_other;
    for (uint32_t index = 0; index < stats.trap_kinds; index++)
        classified_traps += stats.traps[index].count;
    assert(classified_traps == stats.trapped);
    assert(stats.part_inventory_count == 1);
    assert(stats.part_inventory[0].funcs == disc.count);
    assert(stats.part_inventory[0].first_key <=
           stats.part_inventory[0].last_key);

    char *json = read_all("test_emit_generated.report.json");
    char *report = read_all("test_emit_generated.report.txt");
    char *module_json = read_all("test_emit_generated.module.json");
    assert(strstr(json, "\"strictUnsupported\": true"));
    assert(strstr(json, "\"schemaVersion\": 4"));
    assert(strstr(json, "\"structuralReady\": false"));
    assert(strstr(json, "\"pc\": false"));
    assert(strstr(json, "\"unclassified\": 0"));
    assert(strstr(json, "\"traps\": {\"total\": "));
    assert(strstr(json, "\"partitions\""));
    assert(strstr(report, "traps.total="));
    assert(strstr(report, "traps.unclassified=0"));
    assert(strstr(report, "entry.resolved=false"));
    assert(strstr(report, "trap.0.kind="));
    assert(strstr(report, "partition.0.unit=main"));
    assert(strstr(module_json, "\"schemaVersion\": 4"));
    assert(strstr(module_json, "\"abiVersion\": 4"));
    assert(strstr(module_json, "\"id\": null"));
    assert(strstr(module_json, "\"key\": \"0x00001060\""));
    assert(!strstr(module_json, "\"path\""));
    assert(!strstr(module_json, "\"contentHash\""));

    emit_stats repeat_stats;
    const char *repeat_path = "test_emit_repeat.c";
    FILE *repeat = fopen(repeat_path, "wb");
    assert(repeat);
    assert(em_emit(&img, &mod, NULL, &disc, repeat, repeat_path, ".",
                   0, &repeat_stats) == 0);
    fclose(repeat);
    char *repeat_json = read_all("test_emit_repeat.report.json");
    char *repeat_report = read_all("test_emit_repeat.report.txt");
    char *repeat_module_json = read_all("test_emit_repeat.module.json");
    assert(strcmp(json, repeat_json) == 0);
    assert(strcmp(report, repeat_report) == 0);
    assert(strcmp(module_json, repeat_module_json) == 0);

    vm_stub stub;
    vm_var var;
    vm_var_site var_site;
    memset(&stub, 0, sizeof(stub));
    memset(&var, 0, sizeof(var));
    memset(&var_site, 0, sizeof(var_site));
    stub.addr = BASE + 0x90;
    stub.dispatch_key = BASE + 0x90;
    stub.target_site = BASE + 0xA4;
    stub.lib_nid = 0x10203040u;
    stub.func_nid = 0xAABBCCDDu;
    var.lib_nid = 0x11223344u;
    var.var_nid = 0x55667788u;
    var.stub = BASE + 0xA0;
    var.sites = &var_site;
    var.site_count = 1;
    var_site.addr = BASE + 0x90;
    var_site.addend = 8;
    var_site.code = 2;
    mod.have_info = 1;
    mod.metadata_complete = 1;
    /* SNC retail metadata leaves bit zero; generated dispatch remains Thumb. */
    mod.info.module_start = 0x60u;
    mod.info.tls_start = 0x80u;
    mod.info.tls_filesz = 4;
    mod.info.tls_memsz = 8;
    mod.stubs = &stub;
    mod.stub_count = 1;
    mod.total_func_imports = 1;
    mod.vars = &var;
    mod.var_count = 1;
    mod.total_var_imports = 1;

#ifdef _WIN32
    assert(_putenv_s("VITARECOMP_EMIT_PC", "1") == 0);
#else
    assert(setenv("VITARECOMP_EMIT_PC", "1", 1) == 0);
#endif

    emit_stats module_stats;
    const char *module_path = "test_emit_module.c";
    FILE *module_out = fopen(module_path, "wb");
    assert(module_out);
    assert(em_emit_module(&img, &mod, NULL, &disc, module_out, module_path,
                          NULL, "synthetic_mod", 0, &module_stats) == 0);
    fclose(module_out);
    assert(module_stats.entry_present == 1);
    assert(module_stats.entry_raw_address == BASE + 0x60);
    assert(module_stats.entry_dispatch_key == BASE + 0x61);
    assert(module_stats.entry_resolved == 1);
    assert(module_stats.tls_present == 1);
    assert(module_stats.tls_resolved == 1);
    assert(module_stats.tls_alignment == 64);
    assert(module_stats.pc_instrumented == 1);
    assert(module_stats.structural_ready == 1);
    assert(module_stats.production_ready == 0);
    assert(module_stats.dispatch_entries == disc.count + 1);

    char *module_text = read_all(module_path);
    char *module_header = read_all("test_emit_module.h");
    char *module_report = read_all("test_emit_module.report.json");
    char *module_report_text = read_all("test_emit_module.report.txt");
    char *versioned_sidecar = read_all("test_emit_module.module.json");
    assert(strstr(module_text,
                  "void vita_func_synthetic_mod_00001060(void)"));
    assert(!strstr(module_text, "void vita_func_00001060(void)"));
    assert(strstr(module_text,
                  "void vita_register_functions_synthetic_mod(void)"));
    assert(strstr(module_text,
                  "uint32_t vita_var_synthetic_mod_11223344_55667788;"));
    assert(strstr(module_text, "vita_hle_nid_AABBCCDD"));
    assert(strstr(module_text,
                  "vita_fn vita_import_target_synthetic_mod_000010A4 = "
                  "vita_hle_nid_AABBCCDD;"));
    assert(strstr(module_text,
                  "const vita_fn_import "
                  "vita_fn_imports_synthetic_mod[]"));
    assert(strstr(module_text,
                  "\"nid_10203040::nid_AABBCCDD\", 1u"));
    assert(!strstr(module_text, "vita_hle_synthetic_mod"));
    assert(strstr(module_text,
                  "const vita_generated_module_descriptor "
                  "vita_generated_module_synthetic_mod"));
    assert(strstr(module_text,
                  "vita_func_synthetic_mod_00001061"));
    assert(strstr(module_text, "vita_trace_pc(0x00001000u);"));
    assert(strstr(module_header,
                  "extern const vita_generated_module_descriptor "
                  "vita_generated_module_synthetic_mod;"));
    assert(strstr(module_report, "\"structuralReady\": true"));
    assert(strstr(module_report, "\"productionReady\": false"));
    assert(strstr(module_report,
                  "\"instrumentation\": {\"functionEntry\": true, "
                  "\"pc\": true}"));
    assert(strstr(module_report,
                  "\"entry\": {\"present\": true, "
                  "\"rawAddress\": \"0x00001060\", "
                  "\"dispatchKey\": \"0x00001061\", "
                  "\"resolved\": true}"));
    assert(strstr(module_report,
                  "\"tls\": {\"present\": true, \"resolved\": true, "
                  "\"address\": \"0x00001080\", \"fileSize\": 4, "
                  "\"memorySize\": 8, \"alignment\": 64}"));
    assert(strstr(versioned_sidecar,
                  "\"entry\": {\"present\": true, "
                  "\"rawAddress\": \"0x00001060\", "
                  "\"dispatchKey\": \"0x00001061\""));
    assert(strstr(versioned_sidecar,
                  "\"memorySize\": 8, \"alignment\": 64"));
    assert(strstr(versioned_sidecar, "\"id\": \"synthetic_mod\""));
    assert(strstr(versioned_sidecar,
                  "\"ranges\": {\"load\": ["));
    assert(strstr(versioned_sidecar,
                  "\"exports\": {\"count\": 0"));
    assert(strstr(versioned_sidecar, "\"key\": \"0x00001061\""));
    assert(strstr(versioned_sidecar, "\"key\": \"0x00001090\""));
    assert(strstr(versioned_sidecar,
                  "\"key\": \"0x00001090\", \"stubAddress\": "
                  "\"0x00001090\""));
    assert(strstr(versioned_sidecar, "\"kind\": \"functionImport\""));
    assert(strstr(versioned_sidecar,
                  "\"targetSite\": \"0x000010A4\""));
    assert(strstr(versioned_sidecar,
                  "\"targetSlot\": "
                  "\"vita_import_target_synthetic_mod_000010A4\""));
    assert(strstr(versioned_sidecar,
                  "\"name\": \"nid_10203040::nid_AABBCCDD\""));
    assert(strstr(versioned_sidecar,
                  "\"strictUnsupported\": true"));
    assert(count_text(versioned_sidecar, "\"kind\": ") ==
           module_stats.dispatch_entries);
    assert(strstr(versioned_sidecar,
                  "\"libraryNid\": \"0x11223344\""));
    assert(strstr(versioned_sidecar,
                  "\"variableNid\": \"0x55667788\""));
    assert(strstr(versioned_sidecar,
                  "\"address\": \"0x00001090\", \"addend\": 8, "
                  "\"relocationCode\": 2"));
    assert(strstr(versioned_sidecar,
                  "\"pcInstrumentation\": true"));
    assert(!strstr(versioned_sidecar, "\"path\""));
    assert(!strstr(versioned_sidecar, "\"contentHash\""));

    emit_stats module_repeat_stats;
    FILE *module_repeat_out = fopen(module_path, "wb");
    assert(module_repeat_out);
    assert(em_emit_module(&img, &mod, NULL, &disc, module_repeat_out,
                          module_path, NULL, "synthetic_mod", 0,
                          &module_repeat_stats) == 0);
    fclose(module_repeat_out);
    char *module_repeat_text = read_all(module_path);
    char *module_repeat_header = read_all("test_emit_module.h");
    char *module_repeat_report =
        read_all("test_emit_module.report.json");
    char *module_repeat_report_text =
        read_all("test_emit_module.report.txt");
    char *module_repeat_sidecar =
        read_all("test_emit_module.module.json");
    assert(strcmp(module_text, module_repeat_text) == 0);
    assert(strcmp(module_header, module_repeat_header) == 0);
    assert(strcmp(module_report, module_repeat_report) == 0);
    assert(strcmp(module_report_text, module_repeat_report_text) == 0);
    assert(strcmp(versioned_sidecar, module_repeat_sidecar) == 0);
    assert(module_repeat_stats.entry_raw_address ==
           module_stats.entry_raw_address);
    assert(module_repeat_stats.entry_dispatch_key ==
           module_stats.entry_dispatch_key);
    assert(module_repeat_stats.dispatch_entries ==
           module_stats.dispatch_entries);
    assert(module_repeat_stats.trapped == module_stats.trapped);

    {
        const char *compose_inputs[] = {
            "test_emit_module.report.json",
            "test_emit_module.module.json"
        };
        char compose_error[512];
        char *aggregate;
        assert(ac_compose("test_emit_registry.c", "synthetic_mod",
                          compose_inputs, 2, compose_error,
                          sizeof(compose_error)) == 0);
        aggregate = read_all("test_emit_registry.report.json");
        assert(strstr(aggregate, "\"entryEligible\": true"));
        assert(strstr(aggregate, "\"productionReady\": false"));
        free(aggregate);
    }

    free(text);
    free(json);
    free(report);
    free(module_json);
    free(repeat_json);
    free(repeat_report);
    free(repeat_module_json);
    free(module_text);
    free(module_header);
    free(module_report);
    free(module_report_text);
    free(versioned_sidecar);
    free(module_repeat_text);
    free(module_repeat_header);
    free(module_repeat_report);
    free(module_repeat_report_text);
    free(module_repeat_sidecar);
    remove("test_emit_generated.h");
    remove("test_emit_generated.module.json");
    remove("test_emit_generated.report.json");
    remove("test_emit_generated.report.txt");
    remove(outpath);
    remove("test_emit_repeat.h");
    remove("test_emit_repeat.module.json");
    remove("test_emit_repeat.report.json");
    remove("test_emit_repeat.report.txt");
    remove(repeat_path);
    remove("test_emit_module.h");
    remove("test_emit_module.module.json");
    remove("test_emit_module.report.json");
    remove("test_emit_module.report.txt");
    remove(module_path);
    remove("test_emit_registry.c");
    remove("test_emit_registry.h");
    remove("test_emit_registry.report.json");
    remove("test_emit_registry.report.txt");
    printf("emitter prior-vector regressions passed\n");
    return 0;
}
