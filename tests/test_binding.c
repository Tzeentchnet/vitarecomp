/* Public synthetic module-set binding and rollback vectors. */

#include "vitarecomp/binding.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/runtime.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

#define MEMORY_BASE UINT32_C(0x10000000)
#define MEMORY_SIZE UINT32_C(0x00010000)
#define MODULE_A    (MEMORY_BASE + UINT32_C(0x1000))
#define MODULE_B    (MEMORY_BASE + UINT32_C(0x3000))
#define MODULE_SIZE UINT32_C(0x1000)

#define A_FUNCTION  (MODULE_A + UINT32_C(0x101))
#define A_VARIABLE  (MODULE_A + UINT32_C(0x400))
#define B_CROSS     (MODULE_B + UINT32_C(0x101))
#define B_HLE       (MODULE_B + UINT32_C(0x111))
#define B_UNSUPPORTED (MODULE_B + UINT32_C(0x121))
#define B_CROSS_DUP (MODULE_B + UINT32_C(0x131))
#define B_TARGET_0  (MODULE_B + UINT32_C(0x200))
#define B_TARGET_1  (MODULE_B + UINT32_C(0x204))
#define B_TARGET_2  (MODULE_B + UINT32_C(0x208))
#define B_TARGET_3  (MODULE_B + UINT32_C(0x20C))
#define B_PATCH_0   (MODULE_B + UINT32_C(0x300))
#define B_PATCH_1   (MODULE_B + UINT32_C(0x304))
#define PROVIDER_ADDRESS (MEMORY_BASE + UINT32_C(0x8000))

#define FUNCTION_LIBRARY UINT32_C(0x11110000)
#define FUNCTION_NID     UINT32_C(0x11110001)
#define HLE_LIBRARY      UINT32_C(0x22220000)
#define HLE_NID          UINT32_C(0x22220001)
#define UNSUP_LIBRARY    UINT32_C(0x33330000)
#define UNSUP_NID        UINT32_C(0x33330001)
#define VARIABLE_LIBRARY UINT32_C(0x44440000)
#define VARIABLE_NID     UINT32_C(0x44440001)
#define PROVIDER_LIBRARY UINT32_C(0x55550000)
#define PROVIDER_NID     UINT32_C(0x55550001)

typedef struct binding_fixture {
    vita_loader_segment_metadata segments[2];
    vita_loader_module_metadata loaders[2];
    vita_dispatch_entry dispatch_a[2];
    vita_dispatch_entry dispatch_b[4];
    vita_generated_module_descriptor generated[2];
    vita_binding_export_metadata exports_a[3];
    vita_binding_export_metadata exports_b[1];
    vita_binding_function_import_metadata functions_b[4];
    vita_fn_import generated_functions_b[4];
    vita_fn direct_slots[4];
    vita_var_site generated_sites[2];
    vita_var_site metadata_sites[2];
    vita_var_import generated_variables[2];
    vita_binding_variable_import_metadata metadata_variables[2];
    vita_binding_module_metadata post_link[2];
    vita_binding_module modules[2];
    vita_binding_variable_provider providers[1];
    uint32_t exported_value;
    uint32_t provided_value;
} binding_fixture;

static uint32_t g_generated_calls;
static uint32_t g_hle_calls;
static uint32_t g_old_dispatch_calls;

static void generated_function(void) {
    g_generated_calls++;
}

static void placeholder_function(void) {
    assert(!"unresolved import placeholder executed");
}

static void strict_unsupported_function(void) {
    assert(!"strict unsupported target executed");
}

static void registered_hle(void) {
    g_hle_calls++;
}

static void old_dispatch_function(void) {
    g_old_dispatch_calls++;
}

static vita_runtime_config runtime_config(void) {
    vita_runtime_config config;
    memset(&config, 0, sizeof(config));
    config.memory_base = MEMORY_BASE;
    config.memory_size = MEMORY_SIZE;
    return config;
}

static void register_hle(void) {
    const vita_hle_import import = {
        HLE_LIBRARY, HLE_NID, "FixtureHle", "implemented", registered_hle
    };
    assert(vita_hle_register(&import, 1) == VITA_OK);
}

static void write_fixture_words(const binding_fixture *fixture) {
    vita_write32(B_TARGET_0, fixture->functions_b[0].target_address);
    vita_write32(B_TARGET_1, fixture->functions_b[1].target_address);
    vita_write32(B_TARGET_2, fixture->functions_b[2].target_address);
    vita_write32(B_TARGET_3, fixture->functions_b[3].target_address);
    vita_write32(
        B_PATCH_0,
        fixture->metadata_variables[0].target_address +
            fixture->metadata_sites[0].addend);
    vita_write32(
        B_PATCH_1,
        fixture->metadata_variables[1].target_address +
            fixture->metadata_sites[1].addend);
}

static void make_fixture(binding_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));

    fixture->segments[0] = (vita_loader_segment_metadata){
        0, 0, MODULE_A, MODULE_A, MODULE_SIZE, MODULE_SIZE, 7, 4
    };
    fixture->segments[1] = (vita_loader_segment_metadata){
        0, 0, MODULE_B, MODULE_B, MODULE_SIZE, MODULE_SIZE, 7, 4
    };
    fixture->loaders[0] = (vita_loader_module_metadata){
        2, 40, MODULE_A, 0, 1, 0, 1, &fixture->segments[0]
    };
    fixture->loaders[1] = (vita_loader_module_metadata){
        2, 40, MODULE_B, 0, 1, 0, 1, &fixture->segments[1]
    };

    fixture->dispatch_a[0] =
        (vita_dispatch_entry){A_FUNCTION, generated_function};
    fixture->dispatch_b[0] =
        (vita_dispatch_entry){B_CROSS, placeholder_function};
    fixture->dispatch_b[1] =
        (vita_dispatch_entry){B_HLE, placeholder_function};
    fixture->dispatch_b[2] =
        (vita_dispatch_entry){B_UNSUPPORTED, strict_unsupported_function};
    fixture->dispatch_b[3] =
        (vita_dispatch_entry){B_CROSS_DUP, placeholder_function};

    fixture->direct_slots[0] = placeholder_function;
    fixture->direct_slots[1] = placeholder_function;
    fixture->direct_slots[2] = placeholder_function;
    fixture->direct_slots[3] = strict_unsupported_function;
    fixture->generated_functions_b[0] = (vita_fn_import){
        FUNCTION_LIBRARY, FUNCTION_NID, B_CROSS, B_TARGET_0,
        "FixtureGenerated::crossModule", 0, &fixture->direct_slots[0]
    };
    fixture->generated_functions_b[1] = (vita_fn_import){
        FUNCTION_LIBRARY, FUNCTION_NID, B_CROSS_DUP, B_TARGET_1,
        "FixtureGenerated::crossModule", 0, &fixture->direct_slots[1]
    };
    fixture->generated_functions_b[2] = (vita_fn_import){
        HLE_LIBRARY, HLE_NID, B_HLE, B_TARGET_2,
        "FixtureHle::implemented", 0, &fixture->direct_slots[2]
    };
    fixture->generated_functions_b[3] = (vita_fn_import){
        UNSUP_LIBRARY, UNSUP_NID, B_UNSUPPORTED, B_TARGET_3,
        "FixtureUnsupported::namedFailure", 1, &fixture->direct_slots[3]
    };

    fixture->generated_sites[0] =
        (vita_var_site){B_PATCH_0, 4u};
    fixture->generated_sites[1] =
        (vita_var_site){B_PATCH_1, 8u};
    fixture->metadata_sites[0] = fixture->generated_sites[0];
    fixture->metadata_sites[1] = fixture->generated_sites[1];
    fixture->generated_variables[0] = (vita_var_import){
        VARIABLE_LIBRARY, VARIABLE_NID, "Fixture::exported",
        &fixture->exported_value, &fixture->generated_sites[0], 1
    };
    fixture->generated_variables[1] = (vita_var_import){
        PROVIDER_LIBRARY, PROVIDER_NID, "Fixture::provided",
        &fixture->provided_value, &fixture->generated_sites[1], 1
    };

    fixture->generated[0].abi_version =
        VITA_GENERATED_MODULE_ABI_VERSION;
    fixture->generated[0].struct_size =
        (uint32_t)sizeof(fixture->generated[0]);
    fixture->generated[0].module_id = "alpha";
    fixture->generated[0].module_name = "alpha";
    fixture->generated[0].dispatch_entries = fixture->dispatch_a;
    fixture->generated[0].dispatch_count = 1;
    fixture->generated[0].report.strict_unsupported = 1;

    fixture->generated[1].abi_version =
        VITA_GENERATED_MODULE_ABI_VERSION;
    fixture->generated[1].struct_size =
        (uint32_t)sizeof(fixture->generated[1]);
    fixture->generated[1].module_id = "beta";
    fixture->generated[1].module_name = "beta";
    fixture->generated[1].dispatch_entries = fixture->dispatch_b;
    fixture->generated[1].dispatch_count = 4;
    fixture->generated[1].function_imports =
        fixture->generated_functions_b;
    fixture->generated[1].function_import_count = 4;
    fixture->generated[1].variable_imports =
        fixture->generated_variables;
    fixture->generated[1].variable_import_count = 2;
    fixture->generated[1].report.strict_unsupported = 1;

    fixture->exports_a[0] = (vita_binding_export_metadata){
        VITA_BINDING_EXPORT_FUNCTION,
        FUNCTION_LIBRARY, FUNCTION_NID, A_FUNCTION
    };
    fixture->exports_a[1] = (vita_binding_export_metadata){
        VITA_BINDING_EXPORT_VARIABLE,
        VARIABLE_LIBRARY, VARIABLE_NID, A_VARIABLE
    };

    fixture->functions_b[0] = (vita_binding_function_import_metadata){
        FUNCTION_LIBRARY, FUNCTION_NID, B_TARGET_0, A_FUNCTION
    };
    fixture->functions_b[1] = (vita_binding_function_import_metadata){
        FUNCTION_LIBRARY, FUNCTION_NID, B_TARGET_1, A_FUNCTION
    };
    fixture->functions_b[2] = (vita_binding_function_import_metadata){
        HLE_LIBRARY, HLE_NID, B_TARGET_2, UINT32_C(0xF0000001)
    };
    fixture->functions_b[3] = (vita_binding_function_import_metadata){
        UNSUP_LIBRARY, UNSUP_NID, B_TARGET_3, UINT32_C(0xF0000002)
    };

    fixture->metadata_variables[0] =
        (vita_binding_variable_import_metadata){
            VARIABLE_LIBRARY, VARIABLE_NID, A_VARIABLE,
            &fixture->metadata_sites[0], 1
        };
    fixture->metadata_variables[1] =
        (vita_binding_variable_import_metadata){
            PROVIDER_LIBRARY, PROVIDER_NID, PROVIDER_ADDRESS,
            &fixture->metadata_sites[1], 1
        };

    fixture->post_link[0].exports = fixture->exports_a;
    fixture->post_link[0].export_count = 2;
    fixture->post_link[1].function_imports = fixture->functions_b;
    fixture->post_link[1].function_import_count = 4;
    fixture->post_link[1].variable_imports =
        fixture->metadata_variables;
    fixture->post_link[1].variable_import_count = 2;

    fixture->modules[0] = (vita_binding_module){
        &fixture->loaders[0], &fixture->generated[0],
        &fixture->post_link[0]
    };
    fixture->modules[1] = (vita_binding_module){
        &fixture->loaders[1], &fixture->generated[1],
        &fixture->post_link[1]
    };
    fixture->providers[0] = (vita_binding_variable_provider){
        PROVIDER_LIBRARY, PROVIDER_NID, PROVIDER_ADDRESS,
        "Fixture::provided"
    };
    write_fixture_words(fixture);
}

static void assert_error(const vita_binding_error *error,
                         vita_binding_status status,
                         uint32_t module_index,
                         uint32_t record_index) {
    assert(error->status == status);
    assert(error->module_index == module_index);
    assert(error->record_index == record_index);
    assert(strcmp(vita_binding_status_string(status),
                  "unknown binding status") != 0);
}

static void test_checked_dispatch_api(void) {
    static const vita_dispatch_entry valid[] = {
        {UINT32_C(0x1001), generated_function},
        {UINT32_C(0x2001), registered_hle},
    };
    static const vita_dispatch_entry unsorted[] = {
        {UINT32_C(0x2001), registered_hle},
        {UINT32_C(0x1001), generated_function},
    };
    static const vita_dispatch_entry duplicate[] = {
        {UINT32_C(0x1001), generated_function},
        {UINT32_C(0x1001), registered_hle},
    };
    static const vita_dispatch_entry null_function[] = {
        {UINT32_C(0x1001), NULL},
    };
    assert(vita_dispatch_init_checked(NULL, 1) ==
           VITA_DISPATCH_ERROR_NULL_COUNT);
    assert(vita_dispatch_init_checked(valid, 0) ==
           VITA_DISPATCH_ERROR_NULL_COUNT);
    assert(vita_dispatch_init_checked(null_function, 1) ==
           VITA_DISPATCH_ERROR_NULL_FUNCTION);
    assert(vita_dispatch_init_checked(unsorted, 2) ==
           VITA_DISPATCH_ERROR_UNSORTED);
    assert(vita_dispatch_init_checked(duplicate, 2) ==
           VITA_DISPATCH_ERROR_DUPLICATE);
    assert(vita_dispatch_init_checked(valid, 2) == VITA_DISPATCH_OK);
}

static void test_success_and_cross_module_resolution(void) {
    binding_fixture fixture;
    vita_binding_view view = {0};
    vita_binding_error error;
    make_fixture(&fixture);
    fixture.exported_value = UINT32_C(0xDEADBEEF);
    fixture.provided_value = UINT32_C(0xDEADBEEF);

    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_OK);
    assert(error.status == VITA_BINDING_OK);
    assert(view.export_count == 2);
    assert(view.function_import_count == 4);
    assert(view.variable_import_count == 2);
    assert(view.dispatch_count == 5);
    assert(view.function_imports[0].resolution ==
           VITA_BINDING_FUNCTION_GENERATED);
    assert(view.function_imports[1].resolution ==
           VITA_BINDING_FUNCTION_GENERATED);
    assert(view.function_imports[2].resolution ==
           VITA_BINDING_FUNCTION_HLE);
    assert(view.function_imports[3].resolution ==
           VITA_BINDING_FUNCTION_STRICT_UNSUPPORTED);
    assert(fixture.exported_value == A_VARIABLE);
    assert(fixture.provided_value == PROVIDER_ADDRESS);
    assert(vita_read32(B_PATCH_0) == A_VARIABLE + 4u);
    assert(vita_read32(B_PATCH_1) == PROVIDER_ADDRESS + 8u);

    g_generated_calls = 0;
    g_hle_calls = 0;
    vita_call(fixture.direct_slots[0]);
    vita_call(fixture.direct_slots[1]);
    vita_call(fixture.direct_slots[2]);
    vita_dispatch(MODULE_B, B_CROSS);
    vita_dispatch(MODULE_B, B_CROSS_DUP);
    vita_dispatch(MODULE_B, B_HLE);
    assert(g_generated_calls == 4);
    assert(g_hle_calls == 2);
    vita_binding_view_destroy(&view);
}

static void test_duplicates_and_mode_mismatch(void) {
    binding_fixture fixture;
    vita_binding_view view = {0};
    vita_binding_error error;

    make_fixture(&fixture);
    fixture.exports_b[0] = (vita_binding_export_metadata){
        VITA_BINDING_EXPORT_FUNCTION,
        FUNCTION_LIBRARY, FUNCTION_NID, B_CROSS
    };
    fixture.post_link[1].exports = fixture.exports_b;
    fixture.post_link[1].export_count = 1;
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_ERROR_DUPLICATE_EXPORT);
    assert_error(&error, VITA_BINDING_ERROR_DUPLICATE_EXPORT, 1, 0);
    assert(error.related_module_index == 0);

    make_fixture(&fixture);
    fixture.exports_a[2] = (vita_binding_export_metadata){
        VITA_BINDING_EXPORT_FUNCTION,
        0u, UINT32_C(0x77770000), A_FUNCTION
    };
    fixture.post_link[0].export_count = 3;
    fixture.exports_b[0] = (vita_binding_export_metadata){
        VITA_BINDING_EXPORT_FUNCTION,
        0u, UINT32_C(0x77770000), B_CROSS
    };
    fixture.post_link[1].exports = fixture.exports_b;
    fixture.post_link[1].export_count = 1;
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_OK);
    assert(view.export_count == 2);
    vita_binding_view_destroy(&view);

    make_fixture(&fixture);
    fixture.dispatch_b[1].addr = fixture.dispatch_b[0].addr;
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_ERROR_DUPLICATE_DISPATCH);
    assert_error(&error, VITA_BINDING_ERROR_DUPLICATE_DISPATCH, 1, 1);

    make_fixture(&fixture);
    fixture.exports_a[0].address = A_FUNCTION & ~1u;
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_ERROR_MODE_MISMATCH);
    assert_error(&error, VITA_BINDING_ERROR_MODE_MISMATCH, 0, 0);
}

static void test_unresolved_imports(void) {
    binding_fixture fixture;
    vita_binding_view view = {0};
    vita_binding_error error;

    make_fixture(&fixture);
    fixture.functions_b[0].library_nid = UINT32_C(0x66660000);
    fixture.functions_b[0].function_nid = UINT32_C(0x66660001);
    fixture.generated_functions_b[0].lib_nid = UINT32_C(0x66660000);
    fixture.generated_functions_b[0].func_nid = UINT32_C(0x66660001);
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_ERROR_UNRESOLVED_FUNCTION);
    assert_error(&error, VITA_BINDING_ERROR_UNRESOLVED_FUNCTION, 1, 0);

    make_fixture(&fixture);
    fixture.generated_functions_b[3].name = NULL;
    fixture.generated_functions_b[3].strict_unsupported = 0;
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_ERROR_UNRESOLVED_FUNCTION);
    assert_error(&error, VITA_BINDING_ERROR_UNRESOLVED_FUNCTION, 1, 3);

    make_fixture(&fixture);
    fixture.generated_variables[0].lib_nid = UINT32_C(0x77770000);
    fixture.generated_variables[0].var_nid = UINT32_C(0x77770001);
    fixture.metadata_variables[0].library_nid = UINT32_C(0x77770000);
    fixture.metadata_variables[0].variable_nid = UINT32_C(0x77770001);
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_ERROR_UNRESOLVED_VARIABLE);
    assert_error(&error, VITA_BINDING_ERROR_UNRESOLVED_VARIABLE, 1, 0);
}

static void test_bad_patch_sites_and_addends(void) {
    binding_fixture fixture;
    vita_binding_view view = {0};
    vita_binding_error error;

    make_fixture(&fixture);
    fixture.functions_b[0].target_site = MODULE_B + MODULE_SIZE;
    fixture.generated_functions_b[0].target_site =
        MODULE_B + MODULE_SIZE;
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_ERROR_FUNCTION_TARGET_SITE);
    assert_error(&error, VITA_BINDING_ERROR_FUNCTION_TARGET_SITE, 1, 0);

    make_fixture(&fixture);
    fixture.generated_functions_b[0].func_nid ^= 1u;
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_ERROR_FUNCTION_METADATA);
    assert_error(&error, VITA_BINDING_ERROR_FUNCTION_METADATA, 1, 0);

    make_fixture(&fixture);
    fixture.generated_sites[0].addr = MODULE_B + MODULE_SIZE;
    fixture.metadata_sites[0].addr = MODULE_B + MODULE_SIZE;
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_ERROR_PATCH_SITE);
    assert_error(&error, VITA_BINDING_ERROR_PATCH_SITE, 1, 0);
    assert(error.site_index == 0);

    make_fixture(&fixture);
    fixture.generated_sites[0].addend = UINT32_MAX;
    fixture.metadata_sites[0].addend = UINT32_MAX;
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) ==
           VITA_BINDING_ERROR_PATCH_ADDEND_OVERFLOW);
    assert_error(
        &error, VITA_BINDING_ERROR_PATCH_ADDEND_OVERFLOW, 1, 0);
}

static void test_post_link_mismatch_and_rollback(void) {
    static const vita_dispatch_entry old_table[] = {
        {MEMORY_BASE + UINT32_C(0x9001), old_dispatch_function}
    };
    binding_fixture fixture;
    vita_binding_view view = {0};
    vita_binding_error error;

    assert(vita_dispatch_init_checked(old_table, 1) == VITA_DISPATCH_OK);
    make_fixture(&fixture);
    fixture.exported_value = UINT32_C(0xAAAAAAAA);
    fixture.provided_value = UINT32_C(0xBBBBBBBB);
    vita_write32(B_TARGET_0, A_FUNCTION ^ UINT32_C(0x100));
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) ==
           VITA_BINDING_ERROR_FUNCTION_TARGET_MISMATCH);
    assert_error(
        &error, VITA_BINDING_ERROR_FUNCTION_TARGET_MISMATCH, 1, 0);
    assert(fixture.exported_value == UINT32_C(0xAAAAAAAA));
    assert(fixture.provided_value == UINT32_C(0xBBBBBBBB));
    assert(fixture.direct_slots[0] == placeholder_function);
    assert(fixture.direct_slots[1] == placeholder_function);
    assert(fixture.direct_slots[2] == placeholder_function);
    assert(fixture.direct_slots[3] == strict_unsupported_function);
    assert(view.dispatch_count == 0);

    make_fixture(&fixture);
    fixture.exported_value = UINT32_C(0xAAAAAAAA);
    fixture.provided_value = UINT32_C(0xBBBBBBBB);
    vita_write32(B_PATCH_1, UINT32_C(0x12345678));
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) ==
           VITA_BINDING_ERROR_PATCH_VALUE_MISMATCH);
    assert_error(
        &error, VITA_BINDING_ERROR_PATCH_VALUE_MISMATCH, 1, 1);
    assert(fixture.exported_value == UINT32_C(0xAAAAAAAA));
    assert(fixture.provided_value == UINT32_C(0xBBBBBBBB));
    assert(vita_read32(B_PATCH_0) == A_VARIABLE + 4u);
    assert(vita_read32(B_PATCH_1) == UINT32_C(0x12345678));
    assert(fixture.direct_slots[0] == placeholder_function);
    assert(fixture.direct_slots[1] == placeholder_function);
    assert(fixture.direct_slots[2] == placeholder_function);
    assert(fixture.direct_slots[3] == strict_unsupported_function);
    g_old_dispatch_calls = 0;
    vita_dispatch(0, old_table[0].addr);
    assert(g_old_dispatch_calls == 1);
}

static void bind_one_lifecycle_iteration(void) {
    binding_fixture fixture;
    vita_binding_view view = {0};
    vita_binding_error error;
    vita_runtime_config config = runtime_config();
    assert(vita_runtime_init(&config) == VITA_OK);
    register_hle();
    make_fixture(&fixture);
    assert(vita_binding_install_module_set(
               fixture.modules, 2, fixture.providers, 1,
               &view, &error) == VITA_BINDING_OK);
    g_generated_calls = 0;
    vita_dispatch(MODULE_B, B_CROSS);
    assert(g_generated_calls == 1);
    vita_binding_view_destroy(&view);
    vita_runtime_shutdown();
    assert(!vita_runtime_is_initialized());
    assert(vita_dispatch_floor(B_CROSS) == 0);
    assert(vita_dispatch_hits() == 0);
    assert(vita_dispatch_misses() == 0);
    vita_runtime_shutdown();
}

static void test_repeat_init_shutdown(void) {
    bind_one_lifecycle_iteration();
    bind_one_lifecycle_iteration();
}

int main(void) {
    vita_runtime_config config = runtime_config();
    assert(vita_runtime_init(&config) == VITA_OK);
    register_hle();
    test_checked_dispatch_api();
    test_success_and_cross_module_resolution();
    test_duplicates_and_mode_mismatch();
    test_unresolved_imports();
    test_bad_patch_sites_and_addends();
    test_post_link_mismatch_and_rollback();
    vita_runtime_shutdown();

    test_repeat_init_shutdown();
    puts("module-set binding tests passed");
    return 0;
}
