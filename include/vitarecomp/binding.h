/* binding.h -- strict module-set export/import binding. */

#ifndef VITARECOMP_BINDING_H
#define VITARECOMP_BINDING_H

#include "generated_module.h"
#include "loader.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_BINDING_INDEX_NONE UINT32_MAX

typedef enum vita_binding_export_kind {
    VITA_BINDING_EXPORT_FUNCTION = 0,
    VITA_BINDING_EXPORT_VARIABLE = 1,
    VITA_BINDING_EXPORT_TLS_VARIABLE = 2
} vita_binding_export_kind;

/* Retail modules can place data objects in the function prefix of an export
 * table. A non-executable address cannot have a generated dispatch key, so
 * preserve such records as variable exports instead of inventing code. */
static inline vita_binding_export_kind vita_binding_effective_export_kind(
    vita_binding_export_kind declared_kind, int address_is_executable) {
    return declared_kind == VITA_BINDING_EXPORT_FUNCTION &&
                   !address_is_executable
        ? VITA_BINDING_EXPORT_VARIABLE
        : declared_kind;
}

/* Library NID zero is the per-module system-export namespace used for
 * module_start/module_stop/module_exit and module metadata. */
static inline int vita_binding_export_is_global(uint32_t library_nid) {
    return library_nid != 0u;
}

/* Records supplied by the strict module-table/post-link validation extension.
 * Function addresses retain bit 0. `target_site` is the loaded ABS32 word that
 * recorded the linker's selected function target. */
typedef struct vita_binding_export_metadata {
    vita_binding_export_kind kind;
    uint32_t library_nid;
    uint32_t symbol_nid;
    uint32_t address;
} vita_binding_export_metadata;

typedef struct vita_binding_function_import_metadata {
    uint32_t library_nid;
    uint32_t function_nid;
    uint32_t target_site;
    uint32_t target_address;
} vita_binding_function_import_metadata;

/* Only guest ABS32 sites are represented here. MOVW/MOVT imports use the
 * generated value storage and are validated by the strict relocation parser.
 * A zero-site relocation block has no loaded target to inspect, so
 * target_address is zero while the generated storage still resolves normally. */
typedef struct vita_binding_variable_import_metadata {
    uint32_t library_nid;
    uint32_t variable_nid;
    uint32_t target_address;
    const vita_var_site *abs_sites;
    uint32_t abs_count;
} vita_binding_variable_import_metadata;

typedef struct vita_binding_module_metadata {
    const vita_binding_export_metadata *exports;
    uint32_t export_count;
    const vita_binding_function_import_metadata *function_imports;
    uint32_t function_import_count;
    const vita_binding_variable_import_metadata *variable_imports;
    uint32_t variable_import_count;
} vita_binding_module_metadata;

/* `loader` is the metadata produced and checked by the strict loader for the
 * bytes currently in guest memory. `post_link` is produced by the loader's
 * module-table validation extension from that same immutable image. */
typedef struct vita_binding_module {
    const vita_loader_module_metadata *loader;
    const vita_generated_module_descriptor *generated;
    const vita_binding_module_metadata *post_link;
} vita_binding_module;

typedef struct vita_binding_variable_provider {
    uint32_t library_nid;
    uint32_t variable_nid;
    uint32_t address;
    const char *name;
} vita_binding_variable_provider;

typedef enum vita_binding_function_resolution {
    VITA_BINDING_FUNCTION_GENERATED = 0,
    VITA_BINDING_FUNCTION_HLE = 1,
    VITA_BINDING_FUNCTION_STRICT_UNSUPPORTED = 2
} vita_binding_function_resolution;

typedef struct vita_binding_export_view {
    vita_binding_export_kind kind;
    uint32_t library_nid;
    uint32_t symbol_nid;
    uint32_t address;
    uint32_t module_index;
    uint32_t record_index;
} vita_binding_export_view;

typedef struct vita_binding_function_import_view {
    uint32_t library_nid;
    uint32_t function_nid;
    uint32_t dispatch_key;
    uint32_t target_address;
    uint32_t module_index;
    uint32_t record_index;
    vita_binding_function_resolution resolution;
} vita_binding_function_import_view;

typedef struct vita_binding_variable_import_view {
    uint32_t library_nid;
    uint32_t variable_nid;
    uint32_t address;
    uint32_t module_index;
    uint32_t record_index;
} vita_binding_variable_import_view;

typedef struct vita_binding_view {
    vita_binding_export_view *exports;
    uint32_t export_count;
    vita_binding_function_import_view *function_imports;
    uint32_t function_import_count;
    vita_binding_variable_import_view *variable_imports;
    uint32_t variable_import_count;
    uint32_t dispatch_count;
} vita_binding_view;

typedef enum vita_binding_record_kind {
    VITA_BINDING_RECORD_NONE = 0,
    VITA_BINDING_RECORD_MODULE,
    VITA_BINDING_RECORD_DISPATCH,
    VITA_BINDING_RECORD_EXPORT,
    VITA_BINDING_RECORD_FUNCTION_IMPORT,
    VITA_BINDING_RECORD_VARIABLE_IMPORT,
    VITA_BINDING_RECORD_VARIABLE_SITE,
    VITA_BINDING_RECORD_VARIABLE_PROVIDER
} vita_binding_record_kind;

typedef enum vita_binding_status {
    VITA_BINDING_OK = 0,
    VITA_BINDING_ERROR_INVALID_ARGUMENT = -1,
    VITA_BINDING_ERROR_NOT_INITIALIZED = -2,
    VITA_BINDING_ERROR_NO_MEMORY = -3,
    VITA_BINDING_ERROR_GENERATED_ABI = -4,
    VITA_BINDING_ERROR_LOADER_METADATA = -5,
    VITA_BINDING_ERROR_DISPATCH_NULL_COUNT = -6,
    VITA_BINDING_ERROR_DISPATCH_NULL_FUNCTION = -7,
    VITA_BINDING_ERROR_DISPATCH_UNSORTED = -8,
    VITA_BINDING_ERROR_DUPLICATE_DISPATCH = -9,
    VITA_BINDING_ERROR_DUPLICATE_EXPORT = -10,
    VITA_BINDING_ERROR_EXPORT_RANGE = -11,
    VITA_BINDING_ERROR_EXPORT_DISPATCH = -12,
    VITA_BINDING_ERROR_MODE_MISMATCH = -13,
    VITA_BINDING_ERROR_FUNCTION_IMPORT_DISPATCH = -14,
    VITA_BINDING_ERROR_FUNCTION_TARGET_SITE = -15,
    VITA_BINDING_ERROR_FUNCTION_TARGET_MISMATCH = -16,
    VITA_BINDING_ERROR_UNRESOLVED_FUNCTION = -17,
    VITA_BINDING_ERROR_DUPLICATE_VARIABLE_PROVIDER = -18,
    VITA_BINDING_ERROR_VARIABLE_METADATA = -19,
    VITA_BINDING_ERROR_AMBIGUOUS_VARIABLE = -20,
    VITA_BINDING_ERROR_UNRESOLVED_VARIABLE = -21,
    VITA_BINDING_ERROR_VARIABLE_TARGET_MISMATCH = -22,
    VITA_BINDING_ERROR_PATCH_SITE = -23,
    VITA_BINDING_ERROR_PATCH_ADDEND_OVERFLOW = -24,
    VITA_BINDING_ERROR_PATCH_VALUE_MISMATCH = -25,
    VITA_BINDING_ERROR_DISPATCH_INSTALL = -26,
    VITA_BINDING_ERROR_FUNCTION_METADATA = -27
} vita_binding_status;

typedef struct vita_binding_error {
    vita_binding_status status;
    vita_binding_record_kind record_kind;
    uint32_t module_index;
    uint32_t record_index;
    uint32_t site_index;
    uint32_t related_module_index;
    uint32_t related_record_index;
} vita_binding_error;

/* Build one deterministic view, bind every import, and install one merged
 * dispatch table. The output must be zero-initialized and changes only on
 * success. The call is transactional with respect to generated direct-call
 * slots, generated variable storage, and guest ABS32 sites. Binding is an
 * initialization operation: callers must not execute guest code concurrently.
 */
vita_binding_status vita_binding_install_module_set(
    const vita_binding_module *modules,
    size_t module_count,
    const vita_binding_variable_provider *variable_providers,
    size_t variable_provider_count,
    vita_binding_view *view,
    vita_binding_error *error);

void vita_binding_view_destroy(vita_binding_view *view);
const char *vita_binding_status_string(vita_binding_status status);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_BINDING_H */
