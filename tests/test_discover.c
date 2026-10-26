/* test_discover.c — function roots and what discovery makes of stored pointers.
 *
 * Public synthetic module metadata first exercises bounded .ARM.exidx PREL31
 * roots, mode-bearing keys, duplicate reason merging, and explicit rejection.
 *
 * A function nothing calls directly is known only by a word that holds its
 * address, and whether such a word is believed decides whether the function
 * exists in the output. The module here is made by hand: three functions and
 * the words that name them, in the three ways the rules tell apart.
 *
 *   0x10  a destructor that only stores its class's table: MOVW first, no
 *         prologue. Named by the first slot of a two-slot virtual table.
 *   0x20  one with a prologue, named by the second slot.
 *   0x30  a leaf with no prologue, named by one word among other data.
 *   0x40  an accessor between two more, eight bytes that are all printable
 *         ("ldr r0, [r0, #4]; bx lr" is "@hpG"), named the same way.
 *   0x64  the middle of a sentence, named the same way: not a function.
 *
 * In a module without relocations the table is read as a table by the two
 * words every virtual table starts with, and the lone word is weak evidence:
 * walked, a root only if its body is clean. Printable bytes are taken for
 * text unless they return within a few instructions, which text does not.
 * In a module with relocations neither reading is made, because there the
 * relocations say what is a pointer.
 */

#include "analyze.h"
#include "reloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

#define BASE 0x81000000u
#define SIZE 0x200u

static uint8_t g_image[SIZE];
static vm_module g_module;

static void put16(uint32_t at, uint16_t v) {
    g_image[at] = (uint8_t)v;
    g_image[at + 1] = (uint8_t)(v >> 8);
}
static void put32(uint32_t at, uint32_t v) {
    put16(at, (uint16_t)v);
    put16(at + 2, (uint16_t)(v >> 16));
}

static void build(vm_image *img) {
    memset(&g_module, 0, sizeof(g_module));
    for (uint32_t at = 0; at < SIZE; at += 2)
        put16(at, 0xBF00); /* nop */

    /* movw r1, #0x9418; movt r1, #0x818b; adds r1, #8; str r1, [r0]; bx lr */
    put16(0x10, 0xF249);
    put16(0x12, 0x4118);
    put16(0x14, 0xF2C8);
    put16(0x16, 0x118B);
    put16(0x18, 0x3108);
    put16(0x1A, 0x6001);
    put16(0x1C, 0x4770);
    /* push {r4, lr}; pop {r4, pc} */
    put16(0x20, 0xB510);
    put16(0x22, 0xBD10);
    /* movs r0, #0x81; adds r0, #0x90; bx lr */
    put16(0x30, 0x2081);
    put16(0x32, 0x3090);
    put16(0x34, 0x4770);
    /* ldr r0, [r0, #4]; bx lr, three times over */
    for (uint32_t at = 0x3C; at < 0x48; at += 4) {
        put16(at, 0x6840);
        put16(at + 2, 0x4770);
    }
    memcpy(g_image + 0x60, "The quick brown fox", 20);

    /* the virtual table: offset to top, type information, two functions */
    put32(0x100, 0);
    put32(0x104, BASE + 0x180);
    put32(0x108, BASE + 0x10 + 1);
    put32(0x10C, BASE + 0x20 + 1);
    put32(0x110, 0x00000002);
    /* and a structure with one callback in it */
    put32(0x120, 0x12345678);
    put32(0x124, 0x9ABCDEF0);
    put32(0x128, BASE + 0x30 + 1);
    put32(0x12C, 0x00000004);
    put32(0x130, BASE + 0x40 + 1);
    put32(0x134, 0x00000006);
    put32(0x138, BASE + 0x64 + 1);
    put32(0x13C, 0x00000008);

    memset(img, 0, sizeof(*img));
    img->data = g_image;
    img->size = SIZE;
    img->seg_file = 0;
    img->seg_vaddr = BASE;
    img->seg_len = SIZE;
}

/* Why discovery has a function at `addr`: its VF_R_* bits, 0 for none. */
static unsigned reasons(const vf_result *r, uint32_t addr) {
    for (uint32_t i = 0; i < r->count; i++)
        if (r->funcs[i].addr == addr)
            return r->funcs[i].reasons ? r->funcs[i].reasons : 0x8000u;
    return 0;
}

static unsigned mode_reasons(const vf_result *r, uint32_t addr, arm_mode mode) {
    for (uint32_t i = 0; i < r->count; i++)
        if (r->funcs[i].addr == addr && r->funcs[i].mode == mode)
            return r->funcs[i].reasons;
    return 0;
}

static unsigned mode_count(const vf_result *r, uint32_t addr, arm_mode mode) {
    unsigned count = 0;
    for (uint32_t i = 0; i < r->count; i++)
        if (r->funcs[i].addr == addr && r->funcs[i].mode == mode)
            count++;
    return count;
}

static uint32_t prel31(uint32_t place, uint32_t target) {
    int64_t displacement = (int64_t)target - place;
    assert(displacement >= -INT64_C(0x40000000));
    assert(displacement < INT64_C(0x40000000));
    return (uint32_t)displacement & 0x7FFFFFFFu;
}

static void build_module_image(vm_image *img) {
    memset(g_image, 0, sizeof(g_image));
    memset(img, 0, sizeof(*img));
    img->data = g_image;
    img->size = SIZE;
    img->seg_vaddr = BASE;
    img->seg_len = SIZE;
    put16(0x02, 0x0101);
    put32(0x44, UINT32_MAX);
}

static void put_unwind_entry(uint32_t table_offset, uint32_t target) {
    put32(table_offset, prel31(BASE + table_offset, target));
    put32(table_offset + 4, 1u); /* EXIDX_CANTUNWIND */
}

static void test_unwind_roots(void) {
    vm_image img;
    vm_module mod;
    vf_result r;
    char err[128];

    build_module_image(&img);
    put32(0x4C, 0x180);
    put32(0x50, 0x190);
    put16(0x60, 0x4770);      /* Thumb: bx lr */
    put32(0x70, 0xE12FFF1Eu); /* ARM: bx lr */
    put_unwind_entry(0x180, BASE + 0x61);
    put_unwind_entry(0x188, BASE + 0x70);

    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) == 0);
    assert(mod.unwind_entry_count == 2);
    assert(mod.unwind_entries[0].dispatch_key == BASE + 0x61);
    assert(mod.unwind_entries[1].dispatch_key == BASE + 0x70);
    assert(vf_discover(&img, &mod, NULL, &r) == 0);
    assert(r.seeds_unwind == 2);
    assert(mode_reasons(&r, BASE + 0x60, ARM_T32) & VF_R_UNWIND);
    assert(mode_reasons(&r, BASE + 0x70, ARM_A32) & VF_R_UNWIND);
    assert(r.funcs[0].addr == BASE + 0x60 && r.funcs[0].mode == ARM_T32);
    assert(r.funcs[1].addr == BASE + 0x70 && r.funcs[1].mode == ARM_A32);
    vf_free(&r);
    vm_free(&mod);
    printf("  .ARM.exidx mode-bearing roots  ok\n");
}

static void test_unwind_duplicate_reasons(void) {
    vm_image img;
    vm_module mod;
    vf_result r;
    char err[128];

    build_module_image(&img);
    put16(0x60, 0x4770);

    put32(0x24, 0x80);
    put32(0x28, 0xA0);
    put16(0x80, 0x20);
    put16(0x86, 1);
    put32(0x98, BASE + 0xA0);
    put32(0x9C, BASE + 0xA4);
    put32(0xA0, 0x11223344u);
    put32(0xA4, BASE + 0x61);

    put32(0x4C, 0x180);
    put32(0x50, 0x190);
    put_unwind_entry(0x180, BASE + 0x61);
    put_unwind_entry(0x188, BASE + 0x61);

    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) == 0);
    assert(mod.unwind_entry_count == 2);
    assert(vf_discover(&img, &mod, NULL, &r) == 0);
    assert(r.seeds_export == 1);
    assert(r.seeds_unwind == 2);
    assert(mode_count(&r, BASE + 0x60, ARM_T32) == 1);
    assert((mode_reasons(&r, BASE + 0x60, ARM_T32) &
            (VF_R_EXPORT | VF_R_UNWIND)) == (VF_R_EXPORT | VF_R_UNWIND));
    vf_free(&r);
    vm_free(&mod);
    printf("  unwind duplicate reason merge ok\n");
}

static void test_unwind_rejections(void) {
    vm_image img;
    vm_module mod;
    char err[128];

    build_module_image(&img);
    put32(0x4C, 0x180);
    put32(0x50, 0x18C);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) != 0);
    assert(strstr(err, ".ARM.exidx") && strstr(err, "misaligned"));

    build_module_image(&img);
    put32(0x4C, 0x180);
    put32(0x50, SIZE + 8u);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) != 0);
    assert(strstr(err, ".ARM.exidx") && strstr(err, "past"));

    build_module_image(&img);
    put32(0x4C, 0x180);
    put32(0x50, 0x188);
    put_unwind_entry(0x180, BASE + SIZE + 1u);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) != 0);
    assert(strstr(err, ".ARM.exidx") && strstr(err, "outside"));

    build_module_image(&img);
    put32(0x4C, 0x180);
    put32(0x50, 0x188);
    put_unwind_entry(0x180, BASE + 0x62);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) != 0);
    assert(strstr(err, ".ARM.exidx") && strstr(err, "word-aligned"));

    build_module_image(&img);
    put32(0x4C, 0x180);
    put32(0x50, 0x188);
    put32(0x180, 0x80000000u);
    put32(0x184, 1u);
    assert(vm_parse(&img, 0, &mod, err, sizeof(err)) != 0);
    assert(strstr(err, ".ARM.exidx") && strstr(err, "PREL31"));
    printf("  malformed unwind metadata     ok\n");
}

int main(void) {
    vm_image img;
    vf_result r;
    printf("discovery:\n");

    test_unwind_roots();
    test_unwind_duplicate_reasons();
    test_unwind_rejections();

    build(&img);
    assert(vf_discover(&img, &g_module, NULL, &r) == 0);
    assert(reasons(&r, BASE + 0x10) & VF_R_SHAPE); /* by the table's head */
    assert(reasons(&r, BASE + 0x20) & VF_R_SHAPE);
    assert(reasons(&r, BASE + 0x30) & VF_R_WEAK_PTR); /* walked */
    assert(!(reasons(&r, BASE + 0x30) &
             VF_R_ROOTS)); /* and no root by that alone */
    assert(reasons(&r, BASE + 0x40) &
           VF_R_WEAK_PTR);                 /* printable, and it returns */
    assert(reasons(&r, BASE + 0x64) == 0); /* printable, and it does not */
    assert(r.seeds_weak == 2);
    vf_free(&r);
    printf("  a module without relocations   ok\n");

    /* the same bytes in a module whose relocations name its pointers, and name
     * none of these */
    vr_entry none;
    vr_table relocs;
    memset(&none, 0, sizeof(none));
    memset(&relocs, 0, sizeof(relocs));
    none.code = 0xFF;
    relocs.v = &none;
    relocs.n = relocs.cap = 1;
    build(&img);
    assert(vf_discover(&img, &g_module, &relocs, &r) == 0);
    assert(reasons(&r, BASE + 0x10) == 0); /* two words are not a table */
    assert(reasons(&r, BASE + 0x20) &
           VF_R_SHAPE); /* a prologue still is a function */
    assert(reasons(&r, BASE + 0x30) == 0);
    assert(reasons(&r, BASE + 0x40) == 0);
    assert(reasons(&r, BASE + 0x64) == 0);
    assert(r.seeds_weak == 0);
    vf_free(&r);
    printf("  a module with relocations      ok\n");

    /* Two export entries may name one aligned address in different modes.
     * Neither discovery's visited map nor its function index may collapse
     * those into one key. */
    build(&img);
    memset(&g_module, 0, sizeof(g_module));
    g_module.have_info = 1;
    g_module.info.module_start = 0xFFFFFFFFu;
    vm_export exports[2];
    memset(exports, 0, sizeof(exports));
    exports[0].address = BASE + 0x80;
    exports[0].kind = VM_EXPORT_FUNCTION;
    exports[1].address = BASE + 0x81;
    exports[1].kind = VM_EXPORT_FUNCTION;
    g_module.exports = exports;
    g_module.export_count = 2;
    put32(0x80, 0xE12FFF1Eu);
    assert(vf_discover(&img, &g_module, NULL, &r) == 0);
    assert(mode_reasons(&r, BASE + 0x80, ARM_A32) & VF_R_EXPORT);
    assert(mode_reasons(&r, BASE + 0x80, ARM_T32) & VF_R_EXPORT);
    vf_free(&r);
    printf("  ARM/Thumb mode keys            ok\n");

    printf("all discovery tests passed\n");
    return 0;
}
