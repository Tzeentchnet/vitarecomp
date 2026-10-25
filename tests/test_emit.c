/* Synthetic emitter regressions for prior AOT semantic vectors. */

#include "emit.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

#define BASE 0x1000u

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
    disc.funcs = funcs;
    disc.cap = (uint32_t)(sizeof(funcs) / sizeof(funcs[0]));
    img.data = code;
    img.size = sizeof(code);
    img.seg_vaddr = BASE;
    img.seg_len = sizeof(code);
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

    add_func(&disc, BASE + 0x00, ARM_A32);
    add_func(&disc, BASE + 0x10, ARM_A32);
    add_func(&disc, BASE + 0x20, ARM_A32);
    add_func(&disc, BASE + 0x30, ARM_A32);
    add_func(&disc, BASE + 0x40, ARM_A32);
    add_func(&disc, BASE + 0x50, ARM_A32);
    add_func(&disc, BASE + 0x60, ARM_A32);
    add_func(&disc, BASE + 0x60, ARM_T32);
    add_func(&disc, BASE + 0x70, ARM_T32);

#ifdef _WIN32
    assert(_putenv_s("VITARECOMP_EMIT_SPLIT", "0") == 0);
#else
    assert(setenv("VITARECOMP_EMIT_SPLIT", "0", 1) == 0);
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
    assert(stats.part_inventory_count == 1);
    assert(stats.part_inventory[0].funcs == disc.count);

    char *json = read_all("test_emit_generated.report.json");
    char *report = read_all("test_emit_generated.report.txt");
    assert(strstr(json, "\"strictUnsupported\": true"));
    assert(strstr(json, "\"traps\": {\"total\": "));
    assert(strstr(json, "\"partitions\""));
    assert(strstr(report, "traps.total="));
    assert(strstr(report, "trap.0.kind="));
    assert(strstr(report, "partition.0.unit=main"));

    emit_stats repeat_stats;
    const char *repeat_path = "test_emit_repeat.c";
    FILE *repeat = fopen(repeat_path, "wb");
    assert(repeat);
    assert(em_emit(&img, &mod, NULL, &disc, repeat, repeat_path, ".",
                   0, &repeat_stats) == 0);
    fclose(repeat);
    char *repeat_json = read_all("test_emit_repeat.report.json");
    char *repeat_report = read_all("test_emit_repeat.report.txt");
    assert(strcmp(json, repeat_json) == 0);
    assert(strcmp(report, repeat_report) == 0);

    free(text);
    free(json);
    free(report);
    free(repeat_json);
    free(repeat_report);
    remove("test_emit_generated.h");
    remove("test_emit_generated.report.json");
    remove("test_emit_generated.report.txt");
    remove(outpath);
    remove("test_emit_repeat.h");
    remove("test_emit_repeat.report.json");
    remove("test_emit_repeat.report.txt");
    remove(repeat_path);
    printf("emitter prior-vector regressions passed\n");
    return 0;
}
