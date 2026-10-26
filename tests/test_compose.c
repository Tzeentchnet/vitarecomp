/* test_compose.c -- public synthetic multi-image composition vectors. */

#include "compose.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "the tests check with assert(): build them without NDEBUG"
#endif

typedef struct {
    const char *id;
    uint32_t base;
    uint32_t size;
    uint32_t export_nid;
    unsigned named_traps;
    unsigned unclassified_traps;
    unsigned body_cuts;
    unsigned unreported_partitions;
    int pc;
    int entry_resolved;
    int metadata;
    int duplicate_key;
    int unknown_report_member;
    unsigned tls_alignment;
} fixture;

static unsigned fixture_tls_alignment(const fixture *item) {
    return item->metadata
        ? (item->tls_alignment ? item->tls_alignment : 16u)
        : 0u;
}

static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    long length;
    char *text;
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    assert(length >= 0);
    rewind(file);
    text = (char *)malloc((size_t)length + 1);
    assert(text);
    assert(fread(text, 1, (size_t)length, file) == (size_t)length);
    text[length] = '\0';
    assert(fclose(file) == 0);
    return text;
}

static void write_all(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static void replace_all_same_size(const char *path, const char *from,
                                  const char *to) {
    char *text = read_all(path);
    char *at = text;
    FILE *file;
    size_t from_size = strlen(from);
    assert(from_size == strlen(to));
    while ((at = strstr(at, from)) != NULL) {
        memcpy(at, to, from_size);
        at += from_size;
    }
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
    free(text);
}

static void replace_first_same_size(const char *path, const char *from,
                                    const char *to) {
    char *text = read_all(path);
    char *at = strstr(text, from);
    FILE *file;
    assert(at);
    assert(strlen(from) == strlen(to));
    memcpy(at, to, strlen(from));
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
    free(text);
}

static void paths(const fixture *item, char *sidecar, char *report) {
    assert(snprintf(sidecar, 128, "test_compose_%s.module.json", item->id) > 0);
    assert(snprintf(report, 128, "test_compose_%s.report.json", item->id) > 0);
}

static void write_sidecar(const fixture *item) {
    char path[128], report_path[128];
    char descriptor[128], thumb_symbol[128], arm_symbol[128];
    char import_symbol[128], variable_symbol[128];
    char ordered_variable_symbol[128];
    FILE *file;
    unsigned dispatch_count = 2u + (item->metadata ? 1u : 0u) +
                              (item->duplicate_key ? 1u : 0u);
    int structural = item->entry_resolved;
    int production = item->named_traps == 0 &&
                     item->unclassified_traps == 0 &&
                     item->body_cuts == 0 &&
                     item->unreported_partitions == 0;
    paths(item, path, report_path);
    (void)report_path;
    assert(snprintf(descriptor, sizeof(descriptor),
                    "vita_generated_module_%s", item->id) > 0);
    assert(snprintf(thumb_symbol, sizeof(thumb_symbol),
                    "vita_func_%s_%08X", item->id, item->base + 0x101u) > 0);
    assert(snprintf(arm_symbol, sizeof(arm_symbol),
                    "vita_func_%s_%08X", item->id, item->base + 0x100u) > 0);
    assert(snprintf(import_symbol, sizeof(import_symbol),
                    "vita_hle_%s_import", item->id) > 0);
    assert(snprintf(variable_symbol, sizeof(variable_symbol),
                    "vita_var_%s_44556677_8899AABB", item->id) > 0);
    assert(snprintf(ordered_variable_symbol, sizeof(ordered_variable_symbol),
                    "vita_var_%s_FFFFFFFF_00000001", item->id) > 0);

    file = fopen(path, "wb");
    assert(file);
    fprintf(file,
        "{\n"
        "  \"schemaVersion\": 4,\n"
        "  \"abiVersion\": 4,\n"
        "  \"generator\": \"armrecomp\",\n"
        "  \"module\": {\"id\": \"%s\", \"name\": \"%s\", "
        "\"descriptorSymbol\": \"%s\"},\n"
        "  \"ranges\": {\"load\": [\n"
        "    {\"begin\": \"0x%08X\", \"endExclusive\": \"0x%08X\"}\n"
        "  ], \"executable\": [\n"
        "    {\"begin\": \"0x%08X\", \"endExclusive\": \"0x%08X\"}\n"
        "  ]},\n"
        "  \"dispatch\": {\"count\": %u, \"entries\": [\n"
        "    {\"key\": \"0x%08X\", \"kind\": \"function\", "
        "\"symbol\": \"%s\"},\n"
        "    {\"key\": \"0x%08X\", \"kind\": \"function\", "
        "\"symbol\": \"%s\"}",
        item->id, item->id, descriptor,
        item->base, item->base + item->size,
        item->base, item->base + item->size,
        dispatch_count,
        item->base + 0x100u, arm_symbol,
        item->base + 0x101u, thumb_symbol);
    if (item->metadata) {
        fprintf(file,
            ",\n"
            "    {\"key\": \"0x%08X\", \"kind\": \"functionImport\", "
            "\"symbol\": \"%s\", \"libraryNid\": \"0x11223344\", "
            "\"functionNid\": \"0xAABBCCDD\"}",
            item->base + 0x201u, import_symbol);
    }
    if (item->duplicate_key) {
        fprintf(file,
            ",\n"
            "    {\"key\": \"0x%08X\", \"kind\": \"function\", "
            "\"symbol\": \"%s\"}",
            item->base + (item->duplicate_key == 1 ? 0x101u : 0x100u),
            item->duplicate_key == 1 ? thumb_symbol : arm_symbol);
    }
    fprintf(file,
        "\n"
        "  ]},\n"
        "  \"entry\": {\"present\": true, "
        "\"rawAddress\": \"0x%08X\", "
        "\"dispatchKey\": \"0x%08X\", \"function\": ",
        item->base + 0x100u,
        item->base + 0x101u);
    if (item->entry_resolved) fprintf(file, "\"%s\"", thumb_symbol);
    else fputs("null", file);
    fprintf(file,
        ", \"resolved\": %s},\n"
        "  \"tls\": {\"present\": %s, \"resolved\": %s, "
        "\"address\": \"0x%08X\", \"fileSize\": %u, \"memorySize\": %u, "
        "\"alignment\": %u},\n"
        "  \"functionImports\": {\"count\": %u, \"items\": [",
        item->entry_resolved ? "true" : "false",
        item->metadata ? "true" : "false",
        item->metadata ? "true" : "false",
        item->metadata ? item->base + 0x300u : 0u,
        item->metadata ? 4u : 0u,
        item->metadata ? 8u : 0u,
        fixture_tls_alignment(item),
        item->metadata ? 1u : 0u);
    if (item->metadata) {
        fprintf(file,
            "\n"
            "    {\"key\": \"0x%08X\", \"stubAddress\": \"0x%08X\", "
            "\"targetSite\": \"0x%08X\", "
            "\"libraryNid\": \"0x11223344\", "
            "\"functionNid\": \"0xAABBCCDD\", \"name\": null, "
            "\"symbol\": \"%s\", \"targetSlot\": "
            "\"vita_import_target_%s_%08X\", "
            "\"strictUnsupported\": false}\n"
            "  ",
            item->base + 0x201u, item->base + 0x200u,
            item->base + 0x320u, import_symbol, item->id,
            item->base + 0x320u);
    }
    fprintf(file,
        "]},\n"
        "  \"exports\": {\"count\": %u, \"functions\": 1, "
        "\"variables\": %u, \"tlsVariables\": %u, \"items\": [\n"
        "    {\"kind\": \"function\", \"libraryNid\": \"0x%08X\", "
        "\"nid\": \"0x%08X\", \"address\": \"0x%08X\", "
        "\"dispatchKey\": \"0x%08X\"}",
        item->metadata ? 3u : 1u,
        item->metadata ? 1u : 0u,
        item->metadata ? 1u : 0u,
        item->base + 0x50000000u, item->export_nid,
        item->base + 0x100u, item->base + 0x101u);
    if (item->metadata) {
        fprintf(file,
            ",\n"
            "    {\"kind\": \"tlsVariable\", "
            "\"libraryNid\": \"0x%08X\", \"nid\": \"0x05060708\", "
            "\"address\": \"0x%08X\", \"dispatchKey\": null},\n"
            "    {\"kind\": \"variable\", \"libraryNid\": \"0x%08X\", "
            "\"nid\": \"0x01020304\", \"address\": \"0x%08X\", "
            "\"dispatchKey\": null}",
            item->base + 0x50000000u, item->base + 0x330u,
            item->base + 0x50000000u, item->base + 0x320u);
    }
    fprintf(file,
        "\n"
        "  ]},\n"
        "  \"variableImports\": {\"count\": %u, \"items\": [",
        item->metadata ? 2u : 0u);
    if (item->metadata) {
        fprintf(file,
            "\n"
            "    {\"libraryNid\": \"0xFFFFFFFF\", "
            "\"variableNid\": \"0x00000001\", "
            "\"stubAddress\": \"0x%08X\", \"name\": null, "
            "\"symbol\": \"%s\", \"sites\": ["
            "{\"address\": \"0x%08X\", \"addend\": 4, "
            "\"relocationCode\": 2}, "
            "{\"address\": \"0x%08X\", \"addend\": 0, "
            "\"relocationCode\": 2}]},\n"
            "    {\"libraryNid\": \"0x44556677\", "
            "\"variableNid\": \"0x8899AABB\", "
            "\"stubAddress\": \"0x%08X\", \"name\": null, "
            "\"symbol\": \"%s\", \"sites\": ["
            "{\"address\": \"0x%08X\", \"addend\": 8, "
            "\"relocationCode\": 2}]}\n"
            "  ",
            item->base + 0x284u, ordered_variable_symbol,
            item->base + 0x188u, item->base + 0x184u,
            item->base + 0x280u, variable_symbol, item->base + 0x180u);
    }
    fprintf(file,
        "]},\n"
        "  \"report\": {\"schemaVersion\": 4, "
        "\"structuralReady\": %s, \"productionReady\": %s, "
        "\"strictUnsupported\": true, \"pcInstrumentation\": %s, "
        "\"unclassifiedTrapCount\": %u}\n"
        "}\n",
        structural ? "true" : "false",
        production ? "true" : "false",
        item->pc ? "true" : "false",
        item->unclassified_traps);
    assert(fclose(file) == 0);
}

static void write_report(const fixture *item) {
    char sidecar_path[128], path[128];
    FILE *file;
    unsigned trapped = item->named_traps + item->unclassified_traps;
    unsigned translated = 10u - trapped;
    int structural = item->entry_resolved;
    int production = trapped == 0 && item->body_cuts == 0 &&
                     item->unreported_partitions == 0;
    paths(item, sidecar_path, path);
    (void)sidecar_path;
    file = fopen(path, "wb");
    assert(file);
    fprintf(file,
        "{\n"
        "  \"schemaVersion\": 4,\n"
        "  \"generator\": \"armrecomp\",\n"
        "  \"module\": {\"id\": \"%s\", \"name\": \"%s\"},\n"
        "  \"strictUnsupported\": true,\n"
        "  \"structuralReady\": %s,\n"
        "  \"productionReady\": %s,\n"
        "  \"instrumentation\": {\"functionEntry\": true, \"pc\": %s},\n"
        "  \"entry\": {\"present\": true, "
        "\"rawAddress\": \"0x%08X\", "
        "\"dispatchKey\": \"0x%08X\", \"resolved\": %s},\n"
        "  \"tls\": {\"present\": %s, \"resolved\": %s, "
        "\"address\": \"0x%08X\", \"fileSize\": %u, "
        "\"memorySize\": %u, \"alignment\": %u},\n"
        "  \"functions\": {\"emitted\": 2, \"discovered\": 2, "
        "\"dropped\": 0, \"promotedWeakRoots\": 0},\n"
        "  \"instructions\": {\"total\": 10, \"translated\": %u, "
        "\"trapped\": %u, \"bodyCuts\": %u},\n"
        "  \"traps\": {\"total\": %u, \"classified\": %u, "
        "\"unclassified\": %u, \"other\": 0, \"kinds\": [",
        item->id, item->id,
        structural ? "true" : "false",
        production ? "true" : "false",
        item->pc ? "true" : "false",
        item->base + 0x100u,
        item->base + 0x101u,
        item->entry_resolved ? "true" : "false",
        item->metadata ? "true" : "false",
        item->metadata ? "true" : "false",
        item->metadata ? item->base + 0x300u : 0u,
        item->metadata ? 4u : 0u,
        item->metadata ? 8u : 0u,
        fixture_tls_alignment(item),
        translated, trapped, item->body_cuts,
        trapped, item->named_traps, item->unclassified_traps);
    if (item->named_traps) {
        fprintf(file,
            "\n"
            "    {\"kind\": \"synthetic_named_trap\", \"count\": %u}\n"
            "  ",
            item->named_traps);
    }
    fprintf(file,
        "]},\n"
        "  \"partitions\": {\"unreported\": %u, \"items\": [\n"
        "    {\"unit\": \"main\", \"functions\": 2, "
        "\"instructions\": 10, \"firstKey\": \"0x%08X\", "
        "\"lastKey\": \"0x%08X\"}\n"
        "  ]}%s\n"
        "}\n",
        item->unreported_partitions,
        item->base + 0x100u, item->base + 0x101u,
        item->unknown_report_member ? ",\n  \"unknown\": 1" : "");
    assert(fclose(file) == 0);
}

static void write_fixture(const fixture *item) {
    write_sidecar(item);
    write_report(item);
}

static void remove_fixture(const fixture *item) {
    char sidecar[128], report[128];
    paths(item, sidecar, report);
    remove(sidecar);
    remove(report);
}

static int compose(const char *output, const char *entry,
                   const fixture *items, const size_t *order, size_t count,
                   char *error, size_t error_size) {
    const char **inputs = (const char **)calloc(count * 2, sizeof(*inputs));
    char (*paths_storage)[128] =
        (char (*)[128])calloc(count * 2, sizeof(*paths_storage));
    int result;
    assert(inputs && paths_storage);
    for (size_t i = 0; i < count; i++) {
        size_t index = order ? order[i] : i;
        char sidecar[128], report[128];
        paths(&items[index], sidecar, report);
        assert(snprintf(paths_storage[i * 2], 128, "%s", report) > 0);
        assert(snprintf(paths_storage[i * 2 + 1], 128, "%s", sidecar) > 0);
        inputs[i * 2] = paths_storage[i * 2];
        inputs[i * 2 + 1] = paths_storage[i * 2 + 1];
    }
    result = ac_compose(output, entry, inputs, count * 2,
                        error, error_size);
    free(paths_storage);
    free(inputs);
    return result;
}

static void remove_outputs(const char *base) {
    char path[128];
    remove(base);
    assert(snprintf(path, sizeof(path), "%.*s.h",
                    (int)(strrchr(base, '.') - base), base) > 0);
    remove(path);
    assert(snprintf(path, sizeof(path), "%.*s.report.json",
                    (int)(strrchr(base, '.') - base), base) > 0);
    remove(path);
    assert(snprintf(path, sizeof(path), "%.*s.report.txt",
                    (int)(strrchr(base, '.') - base), base) > 0);
    remove(path);
}

static void test_deterministic_composition(void) {
    /* Exact deterministic-non-overlapping-images prior-AOT vector. */
    fixture items[] = {
        {"zeta", 0x3000u, 0x400u, 0x10000003u,
         0, 0, 0, 0, 1, 1, 1, 0, 0},
        {"alpha", 0x1000u, 0x800u, 0x10000001u,
         2, 0, 0, 0, 1, 1, 0, 0, 0},
        {"middle", 0x2000u, 0x400u, 0x10000002u,
         0, 0, 0, 0, 1, 1, 0, 0, 0}
    };
    size_t first_order[] = {0, 2, 1};
    size_t second_order[] = {1, 0, 2};
    const char *output = "test_compose_registry.c";
    char error[512];
    char *first_c, *first_h, *first_json, *first_text;
    char *second_c, *second_h, *second_json, *second_text;
    const char *alpha_pos, *middle_pos, *zeta_pos;
    const char *zeta_module, *zeta_exports, *zeta_tls_export;
    const char *zeta_variable_export, *zeta_variable_imports;
    const char *zeta_high_import, *zeta_low_import;
    const char *zeta_high_site, *zeta_low_site;

    for (size_t i = 0; i < 3; i++) write_fixture(&items[i]);
    assert(compose(output, "alpha", items, first_order, 3,
                   error, sizeof(error)) == 0);
    first_c = read_all("test_compose_registry.c");
    first_h = read_all("test_compose_registry.h");
    first_json = read_all("test_compose_registry.report.json");
    first_text = read_all("test_compose_registry.report.txt");

    assert(compose(output, "alpha", items, second_order, 3,
                   error, sizeof(error)) == 0);
    second_c = read_all("test_compose_registry.c");
    second_h = read_all("test_compose_registry.h");
    second_json = read_all("test_compose_registry.report.json");
    second_text = read_all("test_compose_registry.report.txt");
    assert(strcmp(first_c, second_c) == 0);
    assert(strcmp(first_h, second_h) == 0);
    assert(strcmp(first_json, second_json) == 0);
    assert(strcmp(first_text, second_text) == 0);

    alpha_pos = strstr(first_c, "&vita_generated_module_alpha");
    middle_pos = strstr(first_c, "&vita_generated_module_middle");
    zeta_pos = strstr(first_c, "&vita_generated_module_zeta");
    assert(alpha_pos && middle_pos && zeta_pos);
    assert(alpha_pos < middle_pos && middle_pos < zeta_pos);
    assert(strstr(first_c, "{ 0x00001100u, vita_func_alpha_00001100 }"));
    assert(strstr(first_c, "{ 0x00001101u, vita_func_alpha_00001101 }"));
    assert(strstr(first_c, "vita_generated_image_init_checked"));
    assert(strstr(first_c, "vita_dispatch_init_checked"));
    assert(strstr(first_h,
                  "vita_dispatch_status vita_generated_image_init_checked"));
    assert(strstr(first_c,
                  "(void)vita_generated_image_init_checked();"));
    assert(strstr(first_json, "\"structuralReady\": true"));
    assert(strstr(first_json, "\"schemaVersion\": 4"));
    assert(strstr(first_json,
                  "\"tlsLayout\": {\"moduleCount\": 1, "
                  "\"dataSize\": 8, \"dataAlignment\": 16"));
    assert(strstr(first_c,
                  "vita_generated_image_tls_layout = {"));
    assert(strstr(first_json,
                  "\"rawAddress\": \"0x00001100\", "
                  "\"dispatchKey\": \"0x00001101\""));
    assert(strstr(first_json,
                  "\"entry\": {\"moduleId\": \"alpha\", "
                  "\"rawAddress\": \"0x00001100\", "
                  "\"dispatchKey\": \"0x00001101\""));
    assert(strstr(first_text,
                  "armrecomp composed generation report v4\n"));
    assert(strstr(first_text, "entry.raw_address=0x00001100\n"));
    assert(strstr(first_text, "entry.dispatch_key=0x00001101\n"));
    assert(strstr(first_json, "\"entryEligible\": true"));
    assert(strstr(first_json, "\"productionReady\": false"));
    assert(strstr(first_json,
                  "\"kind\": \"synthetic_named_trap\", \"count\": 2"));
    assert(strstr(first_json, "\"functionImports\": 1"));
    assert(strstr(first_json, "\"functionExports\": 3"));
    assert(strstr(first_json, "\"variableExports\": 1"));
    assert(strstr(first_json, "\"tlsVariableExports\": 1"));
    assert(strstr(first_json, "\"variableImports\": 2"));
    assert(strstr(first_json, "\"variableSites\": 3"));
    zeta_module = strstr(first_json, "\"id\": \"zeta\"");
    zeta_exports = zeta_module ? strstr(zeta_module, "\"exports\"") : NULL;
    zeta_tls_export =
        zeta_exports ? strstr(zeta_exports, "\"nid\": \"0x05060708\"") : NULL;
    zeta_variable_export =
        zeta_exports ? strstr(zeta_exports, "\"nid\": \"0x01020304\"") : NULL;
    zeta_variable_imports =
        zeta_exports ? strstr(zeta_exports, "\"variableImports\"") : NULL;
    zeta_high_import = zeta_variable_imports
        ? strstr(zeta_variable_imports, "\"libraryNid\": \"0xFFFFFFFF\"")
        : NULL;
    zeta_low_import = zeta_variable_imports
        ? strstr(zeta_variable_imports, "\"libraryNid\": \"0x44556677\"")
        : NULL;
    zeta_high_site = zeta_high_import
        ? strstr(zeta_high_import, "\"address\": \"0x00003188\"")
        : NULL;
    zeta_low_site = zeta_high_import
        ? strstr(zeta_high_import, "\"address\": \"0x00003184\"")
        : NULL;
    assert(zeta_tls_export && zeta_variable_export &&
           zeta_tls_export < zeta_variable_export);
    assert(zeta_high_import && zeta_low_import &&
           zeta_high_import < zeta_low_import);
    assert(zeta_high_site && zeta_low_site && zeta_high_site < zeta_low_site);
    assert(!strstr(first_c, "Github"));
    assert(!strstr(first_h, "Github"));
    assert(!strstr(first_text, "Github"));
    assert(!strstr(first_json, "contentHash"));
    assert(!strstr(first_text, "contentHash"));
    assert(!strstr(first_json, "\"path\""));

    free(first_c);
    free(first_h);
    free(first_json);
    free(first_text);
    free(second_c);
    free(second_h);
    free(second_json);
    free(second_text);
    remove_outputs(output);
    for (size_t i = 0; i < 3; i++) remove_fixture(&items[i]);
}

static void test_tls_aggregate_layout(void) {
    fixture items[] = {
        {"tls_a", 0x10000u, 0x400u, 0x41000001u,
         0, 0, 0, 0, 1, 1, 1, 0, 0, 16u},
        {"tls_b", 0x11000u, 0x400u, 0x41000002u,
         0, 0, 0, 0, 1, 1, 1, 0, 0, 64u},
        {"tls_c", 0x12000u, 0x400u, 0x41000003u,
         0, 0, 0, 0, 1, 1, 1, 0, 0, 32u},
    };
    char error[512];
    for (size_t i = 0; i < 3; i++) write_fixture(&items[i]);
    assert(compose("test_compose_tls.c", "tls_a", items, NULL, 3,
                   error, sizeof(error)) == 0);
    char *registry = read_all("test_compose_tls.c");
    char *report = read_all("test_compose_tls.report.json");
    assert(strstr(registry,
                  "{ &vita_generated_module_tls_a, 0u, 0u, 0u, 16u }"));
    assert(strstr(registry,
                  "{ &vita_generated_module_tls_b, 1u, 1u, 64u, 64u }"));
    assert(strstr(registry,
                  "{ &vita_generated_module_tls_c, 2u, 2u, 96u, 32u }"));
    assert(strstr(report,
                  "\"tlsLayout\": {\"moduleCount\": 3, "
                  "\"dataSize\": 104, \"dataAlignment\": 64"));
    assert(strstr(report,
                  "\"moduleId\": \"tls_c\", \"descriptorIndex\": 2, "
                  "\"order\": 2, \"offset\": 96, \"alignment\": 32"));
    free(registry);
    free(report);
    remove_outputs("test_compose_tls.c");

    replace_first_same_size("test_compose_tls_b.module.json",
                            "\"alignment\": 64",
                            "\"alignment\": 32");
    assert(compose("test_compose_tls_bad.c", "tls_a", items, NULL, 3,
                   error, sizeof(error)) != 0);
    assert(strstr(error, "TLS mismatch"));
    remove_outputs("test_compose_tls_bad.c");
    for (size_t i = 0; i < 3; i++) remove_fixture(&items[i]);
}

static void test_rejections(void) {
    char error[512];
    size_t order[] = {0, 1};
    /* Exact reject-overlapping-images prior-AOT vector. */
    fixture overlap[] = {
        {"left", 0x1000u, 0x800u, 0x20000001u,
         0, 0, 0, 0, 1, 1, 0, 0, 0},
        {"right", 0x1700u, 0x900u, 0x20000002u,
         0, 0, 0, 0, 1, 1, 0, 0, 0}
    };
    fixture duplicate_thumb =
        {"duplicate_thumb", 0x5000u, 0x400u, 0x20000003u,
         0, 0, 0, 0, 1, 1, 0, 1, 0};
    fixture duplicate_arm =
        {"duplicate_arm", 0x5800u, 0x400u, 0x20000008u,
         0, 0, 0, 0, 1, 1, 0, 2, 0};
    fixture malformed =
        {"malformed", 0x6000u, 0x400u, 0x20000004u,
         0, 0, 0, 0, 1, 1, 0, 0, 1};
    fixture malformed_sidecar =
        {"malformed_sidecar", 0x6400u, 0x400u, 0x20000009u,
         0, 0, 0, 0, 1, 1, 0, 0, 0};
    fixture malformed_report =
        {"malformed_report", 0x6800u, 0x400u, 0x2000000Au,
         0, 0, 0, 0, 1, 1, 0, 0, 0};
    fixture unresolved =
        {"unresolved", 0x7000u, 0x400u, 0x20000005u,
         0, 0, 0, 0, 1, 0, 0, 0, 0};
    fixture bad_totals =
        {"bad_totals", 0x8000u, 0x400u, 0x20000006u,
         0, 0, 0, 0, 1, 1, 0, 0, 0};
    fixture mode_mismatch =
        {"mode_mismatch", 0x8400u, 0x400u, 0x2000000Bu,
         0, 0, 0, 0, 1, 1, 0, 0, 0};
    fixture ambiguous[] = {
        {"export_left", 0x9000u, 0x400u, 0x20000007u,
         0, 0, 0, 0, 1, 1, 0, 0, 0},
        {"export_right", 0xA000u, 0x400u, 0x20000007u,
         0, 0, 0, 0, 1, 1, 0, 0, 0}
    };
    fixture cross_kind_intra =
        {"cross_kind_intra", 0xA800u, 0x400u, 0x01020304u,
         0, 0, 0, 0, 1, 1, 1, 0, 0};
    fixture cross_kind[] = {
        {"cross_kind_left", 0xB000u, 0x400u, 0x01020304u,
         0, 0, 0, 0, 1, 1, 0, 0, 0},
        {"cross_kind_right", 0xC000u, 0x400u, 0x2000000Cu,
         0, 0, 0, 0, 1, 1, 1, 0, 0}
    };

    write_fixture(&overlap[0]);
    write_fixture(&overlap[1]);
    assert(compose("test_compose_reject.c", "left", overlap, order, 2,
                   error, sizeof(error)) != 0);
    assert(strstr(error, "overlap"));
    remove_fixture(&overlap[0]);
    remove_fixture(&overlap[1]);

    write_fixture(&duplicate_thumb);
    assert(compose("test_compose_reject.c", "duplicate_thumb",
                   &duplicate_thumb, NULL, 1, error, sizeof(error)) != 0);
    assert(strstr(error, "duplicate dispatch key"));
    remove_fixture(&duplicate_thumb);

    write_fixture(&duplicate_arm);
    assert(compose("test_compose_reject.c", "duplicate_arm",
                   &duplicate_arm, NULL, 1, error, sizeof(error)) != 0);
    assert(strstr(error, "duplicate dispatch key"));
    remove_fixture(&duplicate_arm);

    write_fixture(&malformed);
    assert(compose("test_compose_reject.c", "malformed", &malformed, NULL, 1,
                   error, sizeof(error)) != 0);
    assert(strstr(error, "unknown"));
    remove_fixture(&malformed);

    write_fixture(&malformed_sidecar);
    write_all("test_compose_malformed_sidecar.module.json", "{");
    assert(compose("test_compose_reject.c", "malformed_sidecar",
                   &malformed_sidecar, NULL, 1, error, sizeof(error)) != 0);
    remove_fixture(&malformed_sidecar);

    write_fixture(&malformed_report);
    write_all("test_compose_malformed_report.report.json", "[");
    assert(compose("test_compose_reject.c", "malformed_report",
                   &malformed_report, NULL, 1, error, sizeof(error)) != 0);
    remove_fixture(&malformed_report);

    write_fixture(&unresolved);
    assert(compose("test_compose_reject.c", "unresolved", &unresolved, NULL, 1,
                   error, sizeof(error)) != 0);
    assert(strstr(error, "no resolved entry"));
    remove_fixture(&unresolved);

    write_fixture(&bad_totals);
    replace_all_same_size("test_compose_bad_totals.report.json",
                          "\"total\": 0, \"classified\"",
                          "\"total\": 1, \"classified\"");
    assert(compose("test_compose_reject.c", "bad_totals", &bad_totals,
                   NULL, 1, error, sizeof(error)) != 0);
    assert(strstr(error, "trap totals"));
    remove_fixture(&bad_totals);

    {
        char from[64];
        char to[64];
        assert(snprintf(from, sizeof(from),
                        "\"dispatchKey\": \"0x%08X\"",
                        mode_mismatch.base + 0x101u) > 0);
        assert(snprintf(to, sizeof(to),
                        "\"dispatchKey\": \"0x%08X\"",
                        mode_mismatch.base + 0x100u) > 0);
        write_fixture(&mode_mismatch);
        replace_first_same_size("test_compose_mode_mismatch.module.json",
                                from, to);
        replace_first_same_size("test_compose_mode_mismatch.report.json",
                                from, to);
        assert(compose("test_compose_reject.c", "mode_mismatch",
                       &mode_mismatch, NULL, 1, error,
                       sizeof(error)) != 0);
        assert(strstr(error, "mode mismatch"));
        remove_fixture(&mode_mismatch);
    }

    write_fixture(&ambiguous[0]);
    write_fixture(&ambiguous[1]);
    replace_all_same_size("test_compose_export_right.module.json",
                          "0x5000A000", "0x50009000");
    assert(compose("test_compose_reject.c", "export_left", ambiguous,
                   order, 2, error, sizeof(error)) != 0);
    assert(strstr(error, "ambiguous export"));
    replace_all_same_size("test_compose_export_left.module.json",
                          "0x50009000", "0x00000000");
    replace_all_same_size("test_compose_export_right.module.json",
                          "0x50009000", "0x00000000");
    assert(compose("test_compose_reject.c", "export_left", ambiguous,
                   order, 2, error, sizeof(error)) == 0);
    remove_fixture(&ambiguous[0]);
    remove_fixture(&ambiguous[1]);

    write_fixture(&cross_kind_intra);
    assert(compose("test_compose_reject.c", "cross_kind_intra",
                   &cross_kind_intra, NULL, 1, error,
                   sizeof(error)) != 0);
    assert(strstr(error, "ambiguous export"));
    remove_fixture(&cross_kind_intra);

    write_fixture(&cross_kind[0]);
    write_fixture(&cross_kind[1]);
    replace_all_same_size("test_compose_cross_kind_right.module.json",
                          "0x5000C000", "0x5000B000");
    assert(compose("test_compose_reject.c", "cross_kind_left",
                   cross_kind, order, 2, error, sizeof(error)) != 0);
    assert(strstr(error, "ambiguous export"));
    remove_fixture(&cross_kind[0]);
    remove_fixture(&cross_kind[1]);
    remove_outputs("test_compose_reject.c");
}

static void test_structural_blockers(void) {
    fixture blocked[] = {
        {"body_cut", 0xB000u, 0x400u, 0x30000001u,
         0, 0, 1, 0, 1, 1, 0, 0, 0},
        {"unreported", 0xC000u, 0x400u, 0x30000002u,
         0, 0, 0, 1, 1, 1, 0, 0, 0},
        {"unclassified", 0xD000u, 0x400u, 0x30000003u,
         0, 1, 0, 0, 1, 1, 0, 0, 0},
        {"missing_pc", 0xE000u, 0x400u, 0x30000004u,
         0, 0, 0, 0, 0, 1, 0, 0, 0}
    };
    const char *needles[] = {
        "\"bodyCuts\": 1",
        "\"unreported\": 1",
        "\"unclassified\": 1",
        "\"pc\": false"
    };
    char error[512];
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        char *json;
        write_fixture(&blocked[i]);
        assert(compose("test_compose_blocked.c", blocked[i].id,
                       &blocked[i], NULL, 1, error, sizeof(error)) == 0);
        json = read_all("test_compose_blocked.report.json");
        assert(strstr(json, "\"structuralReady\": false"));
        assert(strstr(json, "\"entryEligible\": false"));
        assert(strstr(json, i == 3 ? "\"productionReady\": true"
                                  : "\"productionReady\": false"));
        assert(strstr(json, needles[i]));
        free(json);
        remove_fixture(&blocked[i]);
    }
    remove_outputs("test_compose_blocked.c");
}

int main(void) {
    test_deterministic_composition();
    test_tls_aggregate_layout();
    test_rejections();
    test_structural_blockers();
    puts("generated image composition tests passed");
    return 0;
}
