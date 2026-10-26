/* generated_module.h -- versioned ABI exported by generated modules. */

#ifndef VITARECOMP_GENERATED_MODULE_H
#define VITARECOMP_GENERATED_MODULE_H

#include "dispatch.h"

#include <stdint.h>

#define VITA_GENERATED_MODULE_ABI_VERSION 4u
#define VITA_GENERATED_REPORT_SCHEMA_VERSION 4u

#define VITA_GENERATED_INSTRUMENT_FUNCTION_ENTRY (1u << 0)
#define VITA_GENERATED_INSTRUMENT_PC             (1u << 1)

/* A variable the module imports from a firmware library. The generated code
 * reads its guest address through `value`; the loader writes that address
 * into the image at every word that held a placeholder (`abs_sites`). */
typedef struct {
    uint32_t addr;
    uint32_t addend;
} vita_var_site;

typedef struct {
    uint32_t             lib_nid;
    uint32_t             var_nid;
    const char          *name;        /* "SceLibc::_Tolotab", or NULL */
    uint32_t            *value;
    const vita_var_site *abs_sites;
    uint32_t             abs_count;
} vita_var_import;

/* A function import as emitted from one module's import table. `dispatch_key`
 * is always the local, mode-bearing guest stub key. `target_site` is the
 * address of the import-table word the loader/linker updates; its loaded value
 * is deliberately not part of this descriptor. Generated direct calls read
 * `target_slot`, while the binder installs that same resolved function at the
 * local dispatch key for indirect calls. */
typedef struct {
    uint32_t  lib_nid;
    uint32_t  func_nid;
    uint32_t  dispatch_key;
    uint32_t  target_site;
    const char *name; /* "SceLibc::memcpy", or NULL when unresolved */
    uint32_t  strict_unsupported;
    vita_fn  *target_slot;
} vita_fn_import;

typedef struct {
    uint32_t present;
    uint32_t resolved;
    uint32_t address;
    uint32_t file_size;
    uint32_t memory_size;
    uint32_t alignment;
} vita_generated_module_tls;

/* A compact copy of deterministic generation-report fields for generated
 * registries and runtime consumers. The composer validates the full JSON
 * report before referencing this descriptor. */
typedef struct {
    uint32_t schema_version;
    uint32_t structural_ready;
    uint32_t production_ready;
    uint32_t strict_unsupported;
    uint32_t instrumentation_flags;
    uint32_t trapped_instructions;
    uint32_t unclassified_traps;
    uint32_t body_cuts;
    uint32_t unreported_partitions;
} vita_generated_module_report;

/* SNC retail metadata commonly leaves module_start bit zero even though the
 * entry is Thumb. Preserve that raw address for agreement checks, but always
 * use this normalized key for generated dispatch and function symbols. */
static inline uint32_t
vita_module_start_dispatch_key(uint32_t raw_address) {
    return (raw_address & ~UINT32_C(1)) | UINT32_C(1);
}

/* `abi_version` and `struct_size` are the first fields so consumers can reject
 * an incompatible descriptor before reading any later field. A module ID is
 * NULL only for legacy armrecomp invocations; such output is intentionally not
 * structurally ready for multi-image composition. */
typedef struct vita_generated_module_descriptor {
    uint32_t                           abi_version;
    uint32_t                           struct_size;
    const char                        *module_id;
    const char                        *module_name;
    const vita_dispatch_entry         *dispatch_entries;
    uint32_t                           dispatch_count;
    uint32_t                           entry_present;
    uint32_t                           entry_raw_address;
    uint32_t                           entry_dispatch_key;
    vita_fn                            entry_function;
    uint32_t                           entry_resolved;
    vita_generated_module_tls          tls;
    const vita_fn_import              *function_imports;
    uint32_t                           function_import_count;
    const vita_var_import             *variable_imports;
    uint32_t                           variable_import_count;
    vita_generated_module_report       report;
} vita_generated_module_descriptor;

/* Composition fixes the order in which TLS-bearing descriptors contribute to
 * the process TLS image. The registry carries the independently checkable
 * expected offset/alignment for every such descriptor. */
typedef struct vita_generated_tls_layout_entry {
    const vita_generated_module_descriptor *module;
    uint32_t descriptor_index;
    uint32_t tls_order;
    uint32_t expected_offset;
    uint32_t expected_alignment;
} vita_generated_tls_layout_entry;

typedef struct vita_generated_tls_layout {
    uint32_t abi_version;
    uint32_t struct_size;
    const vita_generated_tls_layout_entry *entries;
    uint32_t descriptor_count;
    uint32_t tls_module_count;
    uint32_t data_size;
    uint32_t data_alignment;
} vita_generated_tls_layout;

#endif /* VITARECOMP_GENERATED_MODULE_H */
