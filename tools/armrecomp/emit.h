/* emit.h — turning discovered functions into C. */

#ifndef ARMRECOMP_EMIT_H
#define ARMRECOMP_EMIT_H

#include "analyze.h"
#include "nids.h"

#include <stdio.h>

/* What is still trapping, by kind.
 *
 * "31% untranslated" is not actionable; "MOVT is 40% of what is left" is. This
 * table is what turns the question of where to spend effort next from a guess
 * into a ranked list — and it is how MOVT was found to be worth far more than
 * its instruction count suggested. */
#define EM_MAX_TRAP_KINDS 64
#define EM_MAX_PARTS 256
#define EM_MODULE_ID_MAX 63

typedef struct {
    const char *what;      /* mnemonic, or the class when unnamed */
    uint32_t    count;
} em_trap_kind;

typedef struct {
    uint32_t index;
    uint32_t funcs;
    uint32_t insns;
    uint32_t first_key;
    uint32_t last_key;
    uint8_t  auxiliary;
} em_part;

typedef struct {
    uint32_t funcs;             /* emitted                                */
    uint32_t funcs_discovered;  /* registered by discovery                */
    uint32_t funcs_dropped;     /* of those, reachable from no root       */
    uint32_t funcs_promoted;    /* weak pointer targets made roots by a clean body */
    uint32_t parts;             /* translation units the functions went into */
    uint32_t bodies_cut;        /* walks that ended without reaching a return */
    uint32_t back_edges;        /* backward branches: each counts the time slice */
    uint32_t wait_loops;        /* of those, loops that only wait on memory      */
    uint32_t part_funcs;        /* functions in the largest of them           */
    uint32_t insns;
    uint32_t translated;   /* emitted as real C            */
    uint32_t trapped;      /* emitted as a named trap      */
    uint32_t literals;     /* literal-pool loads folded to constants */
    uint32_t import_calls; /* call sites bound to a firmware import  */
    uint32_t imports_used; /* distinct imports actually reached      */
    uint32_t var_imports;  /* variables the module imports           */
    uint32_t var_refs;     /* sites in the code that read one        */
    uint32_t dispatch_entries;

    uint32_t unclassified_traps;
    uint32_t pc_instrumented;
    uint32_t entry_present;
    uint32_t entry_raw_address;
    uint32_t entry_dispatch_key;
    uint32_t entry_resolved;
    uint32_t tls_present;
    uint32_t tls_resolved;
    uint32_t tls_address;
    uint32_t tls_filesz;
    uint32_t tls_memsz;
    uint32_t tls_alignment;
    uint32_t structural_ready;
    uint32_t production_ready;

    /* Addresses discovery registered as functions that are really import
     * stubs. The stub wins in the dispatch table; this counts how often
     * discovery walked into the import table and believed what it found. */
    uint32_t stub_shadowed;

    em_trap_kind traps[EM_MAX_TRAP_KINDS];
    uint32_t     trap_kinds;
    uint32_t     traps_other;   /* beyond the table's capacity */

    em_part      part_inventory[EM_MAX_PARTS];
    uint32_t     part_inventory_count;
    uint32_t     parts_unreported;
} emit_stats;

/* Emit discovered functions to `out`. `limit` caps the number emitted (0 for
 * all), which matters for bring-up: 19k functions is 133 MB of C, and getting
 * a few hundred to compile and link proves the translation is valid C long
 * before the whole module is worth building.
 *
 * `impdir` receives ONE FILE PER IMPORT, which is not tidiness — it is what
 * makes an import overridable. The defaults are meant to be built into a
 * static library and listed after the runtime, and a linker pulls an archive
 * member only for a symbol still undefined. At one member per function a real
 * implementation simply wins. Put all 524 in a single translation unit and the
 * ~500 unimplemented ones drag the whole object in, taking the implemented
 * definitions with them and colliding with every real one. */
/* `outpath` names the main file `out` was opened on; the parts and the
 * header that declares everything are written beside it, as
 * <base>_NNN.c and <base>.h. VITARECOMP_EMIT_SPLIT in the environment sets
 * the functions per part (default 2000; 0 keeps everything in `out`).
 * Deterministic <base>.report.json and <base>.report.txt files contain the
 * unsupported/trap census and translation-unit inventory. <base>.module.json
 * is the path-free per-module ABI sidecar, including load/executable ranges
 * and complete dispatch, export, import, TLS, and variable metadata. */
int em_emit(const vm_image *img, const vm_module *mod, const nid_db *db,
            const vf_result *disc, FILE *out, const char *outpath, const char *impdir,
            uint32_t limit, emit_stats *st);

/* Versioned emission. `module_id` must pass em_module_id_valid(); when present
 * it is included in every module-owned external symbol. Passing NULL is exactly
 * the legacy em_emit() naming mode. */
int em_emit_module(const vm_image *img, const vm_module *mod, const nid_db *db,
                   const vf_result *disc, FILE *out, const char *outpath,
                   const char *impdir, const char *module_id, uint32_t limit,
                   emit_stats *st);

int em_module_id_valid(const char *module_id);

#endif /* ARMRECOMP_EMIT_H */
