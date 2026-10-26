/* binding.c -- deterministic strict binding for a loaded generated module set. */

#include "vitarecomp/binding.h"

#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/mem.h"
#include "excl_internal.h"
#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>

typedef struct binding_dispatch {
    vita_dispatch_entry entry;
    uint32_t module_index;
    uint32_t record_index;
    int import_claimed;
} binding_dispatch;

typedef struct binding_provider {
    vita_binding_variable_provider provider;
    uint32_t record_index;
} binding_provider;

typedef struct binding_function {
    const vita_binding_function_import_metadata *metadata;
    const vita_fn_import *generated;
    uint32_t module_index;
    uint32_t record_index;
    vita_fn function;
    vita_fn previous;
    vita_binding_function_resolution resolution;
} binding_function;

typedef struct binding_storage {
    uint32_t *pointer;
    uint32_t value;
    uint32_t previous;
    uint32_t module_index;
    uint32_t record_index;
} binding_storage;

typedef struct binding_patch {
    uint32_t address;
    uint32_t value;
    uint32_t previous;
    uint32_t module_index;
    uint32_t record_index;
    uint32_t site_index;
} binding_patch;

static once_flag g_binding_once = ONCE_FLAG_INIT;
static mtx_t g_binding_lock;

static void binding_once(void) {
    (void)mtx_init(&g_binding_lock, mtx_plain);
}

static void clear_error(vita_binding_error *error) {
    if (!error) return;
    memset(error, 0, sizeof(*error));
    error->status = VITA_BINDING_OK;
    error->module_index = VITA_BINDING_INDEX_NONE;
    error->record_index = VITA_BINDING_INDEX_NONE;
    error->site_index = VITA_BINDING_INDEX_NONE;
    error->related_module_index = VITA_BINDING_INDEX_NONE;
    error->related_record_index = VITA_BINDING_INDEX_NONE;
}

static vita_binding_status binding_fail(
    vita_binding_error *error,
    vita_binding_status status,
    vita_binding_record_kind record_kind,
    uint32_t module_index,
    uint32_t record_index,
    uint32_t site_index) {
    clear_error(error);
    if (error) {
        error->status = status;
        error->record_kind = record_kind;
        error->module_index = module_index;
        error->record_index = record_index;
        error->site_index = site_index;
    }
    return status;
}

static void set_related(vita_binding_error *error, uint32_t module_index,
                        uint32_t record_index) {
    if (!error) return;
    error->related_module_index = module_index;
    error->related_record_index = record_index;
}

static int exact_pointer_count(const void *pointer, uint32_t count) {
    return (pointer != NULL) == (count != 0u);
}

static int empty_view(const vita_binding_view *view) {
    return view && !view->exports && !view->export_count &&
           !view->function_imports && !view->function_import_count &&
           !view->variable_imports && !view->variable_import_count &&
           !view->dispatch_count;
}

static int add_count(size_t *total, uint32_t count) {
    if ((size_t)count > SIZE_MAX - *total) return 0;
    *total += count;
    return *total <= UINT32_MAX;
}

static int loader_metadata_valid(
    const vita_loader_module_metadata *metadata) {
    if (!metadata || !metadata->segment_count || !metadata->segments)
        return 0;
    for (uint32_t i = 0; i < metadata->segment_count; i++) {
        const vita_loader_segment_metadata *segment = &metadata->segments[i];
        const uint64_t end =
            (uint64_t)segment->virtual_address + segment->memory_size;
        if (!segment->memory_size || segment->file_size > segment->memory_size ||
            end > UINT64_C(0x100000000))
            return 0;
        for (uint32_t k = 0; k < i; k++) {
            const vita_loader_segment_metadata *prior =
                &metadata->segments[k];
            const uint64_t prior_end =
                (uint64_t)prior->virtual_address + prior->memory_size;
            if ((uint64_t)segment->virtual_address < prior_end &&
                (uint64_t)prior->virtual_address < end)
                return 0;
        }
    }
    return 1;
}

static int loader_contains(const vita_loader_module_metadata *metadata,
                           uint32_t address, uint32_t size) {
    const uint64_t end = (uint64_t)address + size;
    if (!size || end > UINT64_C(0x100000000)) return 0;
    for (uint32_t i = 0; i < metadata->segment_count; i++) {
        const vita_loader_segment_metadata *segment = &metadata->segments[i];
        const uint64_t segment_end =
            (uint64_t)segment->virtual_address + segment->memory_size;
        if (address >= segment->virtual_address && end <= segment_end)
            return 1;
    }
    return 0;
}

static int dispatch_order(const void *left, const void *right) {
    const binding_dispatch *a = (const binding_dispatch *)left;
    const binding_dispatch *b = (const binding_dispatch *)right;
    if (a->entry.addr < b->entry.addr) return -1;
    if (a->entry.addr > b->entry.addr) return 1;
    if (a->module_index < b->module_index) return -1;
    if (a->module_index > b->module_index) return 1;
    if (a->record_index < b->record_index) return -1;
    if (a->record_index > b->record_index) return 1;
    return 0;
}

static int export_order(const void *left, const void *right) {
    const vita_binding_export_view *a =
        (const vita_binding_export_view *)left;
    const vita_binding_export_view *b =
        (const vita_binding_export_view *)right;
    if (a->library_nid < b->library_nid) return -1;
    if (a->library_nid > b->library_nid) return 1;
    if (a->symbol_nid < b->symbol_nid) return -1;
    if (a->symbol_nid > b->symbol_nid) return 1;
    if (a->module_index < b->module_index) return -1;
    if (a->module_index > b->module_index) return 1;
    if (a->record_index < b->record_index) return -1;
    if (a->record_index > b->record_index) return 1;
    return 0;
}

static int provider_order(const void *left, const void *right) {
    const binding_provider *a = (const binding_provider *)left;
    const binding_provider *b = (const binding_provider *)right;
    if (a->provider.library_nid < b->provider.library_nid) return -1;
    if (a->provider.library_nid > b->provider.library_nid) return 1;
    if (a->provider.variable_nid < b->provider.variable_nid) return -1;
    if (a->provider.variable_nid > b->provider.variable_nid) return 1;
    if (a->record_index < b->record_index) return -1;
    if (a->record_index > b->record_index) return 1;
    return 0;
}

static int function_view_order(const void *left, const void *right) {
    const vita_binding_function_import_view *a =
        (const vita_binding_function_import_view *)left;
    const vita_binding_function_import_view *b =
        (const vita_binding_function_import_view *)right;
    if (a->library_nid < b->library_nid) return -1;
    if (a->library_nid > b->library_nid) return 1;
    if (a->function_nid < b->function_nid) return -1;
    if (a->function_nid > b->function_nid) return 1;
    if (a->module_index < b->module_index) return -1;
    if (a->module_index > b->module_index) return 1;
    if (a->record_index < b->record_index) return -1;
    if (a->record_index > b->record_index) return 1;
    return 0;
}

static int variable_view_order(const void *left, const void *right) {
    const vita_binding_variable_import_view *a =
        (const vita_binding_variable_import_view *)left;
    const vita_binding_variable_import_view *b =
        (const vita_binding_variable_import_view *)right;
    if (a->library_nid < b->library_nid) return -1;
    if (a->library_nid > b->library_nid) return 1;
    if (a->variable_nid < b->variable_nid) return -1;
    if (a->variable_nid > b->variable_nid) return 1;
    if (a->module_index < b->module_index) return -1;
    if (a->module_index > b->module_index) return 1;
    if (a->record_index < b->record_index) return -1;
    if (a->record_index > b->record_index) return 1;
    return 0;
}

static int patch_order(const void *left, const void *right) {
    const binding_patch *a = (const binding_patch *)left;
    const binding_patch *b = (const binding_patch *)right;
    if (a->address < b->address) return -1;
    if (a->address > b->address) return 1;
    if (a->module_index < b->module_index) return -1;
    if (a->module_index > b->module_index) return 1;
    if (a->record_index < b->record_index) return -1;
    if (a->record_index > b->record_index) return 1;
    if (a->site_index < b->site_index) return -1;
    if (a->site_index > b->site_index) return 1;
    return 0;
}

static binding_dispatch *find_dispatch(binding_dispatch *items, size_t count,
                                       uint32_t key) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (items[middle].entry.addr == key) return &items[middle];
        if (items[middle].entry.addr < key) low = middle + 1;
        else high = middle;
    }
    return NULL;
}

static binding_dispatch *find_aligned_dispatch(binding_dispatch *items,
                                               size_t count, uint32_t key) {
    const uint32_t aligned = key & ~1u;
    binding_dispatch *arm = find_dispatch(items, count, aligned);
    if (arm) return arm;
    return find_dispatch(items, count, aligned | 1u);
}

static vita_binding_export_view *find_export(
    vita_binding_export_view *items, size_t count,
    uint32_t library_nid, uint32_t symbol_nid) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        vita_binding_export_view *item = &items[middle];
        if (item->library_nid == library_nid &&
            item->symbol_nid == symbol_nid)
            return item;
        if (item->library_nid < library_nid ||
            (item->library_nid == library_nid &&
             item->symbol_nid < symbol_nid))
            low = middle + 1;
        else
            high = middle;
    }
    return NULL;
}

static binding_provider *find_provider(binding_provider *items, size_t count,
                                       uint32_t library_nid,
                                       uint32_t variable_nid) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        binding_provider *item = &items[middle];
        if (item->provider.library_nid == library_nid &&
            item->provider.variable_nid == variable_nid)
            return item;
        if (item->provider.library_nid < library_nid ||
            (item->provider.library_nid == library_nid &&
             item->provider.variable_nid < variable_nid))
            low = middle + 1;
        else
            high = middle;
    }
    return NULL;
}

static int same_variable_key(const vita_var_import *generated,
                             const vita_binding_variable_import_metadata *meta) {
    return generated->lib_nid == meta->library_nid &&
           generated->var_nid == meta->variable_nid;
}

static int same_function_record(
    const vita_fn_import *generated,
    const vita_binding_function_import_metadata *metadata) {
    return generated->lib_nid == metadata->library_nid &&
           generated->func_nid == metadata->function_nid &&
           generated->target_site == metadata->target_site;
}

static int find_metadata_site(
    const vita_binding_variable_import_metadata *metadata,
    const vita_var_site *site,
    uint32_t *index) {
    for (uint32_t i = 0; i < metadata->abs_count; i++) {
        if (metadata->abs_sites[i].addr == site->addr &&
            metadata->abs_sites[i].addend == site->addend) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

void vita_binding_view_destroy(vita_binding_view *view) {
    if (!view) return;
    free(view->exports);
    free(view->function_imports);
    free(view->variable_imports);
    memset(view, 0, sizeof(*view));
}

void vita_binding_runtime_reset(void) {
    call_once(&g_binding_once, binding_once);
    mtx_lock(&g_binding_lock);
    vita_dispatch_reset();
    mtx_unlock(&g_binding_lock);
}

vita_binding_status vita_binding_install_module_set(
    const vita_binding_module *modules,
    size_t module_count,
    const vita_binding_variable_provider *variable_providers,
    size_t variable_provider_count,
    vita_binding_view *view,
    vita_binding_error *error) {
    vita_binding_status status = VITA_BINDING_OK;
    vita_binding_view result;
    binding_dispatch *dispatch = NULL;
    vita_dispatch_entry *dispatch_table = NULL;
    binding_provider *providers = NULL;
    binding_function *functions = NULL;
    binding_storage *storages = NULL;
    binding_patch *patches = NULL;
    size_t dispatch_count = 0, export_count = 0, function_count = 0;
    size_t variable_count = 0, patch_count = 0;
    size_t dispatch_cursor = 0, export_cursor = 0, function_cursor = 0;
    size_t variable_cursor = 0, patch_cursor = 0;
    int memory_locked = 0;
    int mutations_applied = 0;

    memset(&result, 0, sizeof(result));
    clear_error(error);
    call_once(&g_binding_once, binding_once);
    mtx_lock(&g_binding_lock);

    if (!vita_runtime_can_bind()) {
        status = binding_fail(
            error, VITA_BINDING_ERROR_NOT_INITIALIZED,
            VITA_BINDING_RECORD_NONE, VITA_BINDING_INDEX_NONE,
            VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
        goto fail;
    }
    if (!modules || !module_count || module_count > UINT32_MAX ||
        !empty_view(view) ||
        ((variable_providers != NULL) != (variable_provider_count != 0u)) ||
        variable_provider_count > UINT32_MAX) {
        status = binding_fail(
            error, VITA_BINDING_ERROR_INVALID_ARGUMENT,
            VITA_BINDING_RECORD_NONE, VITA_BINDING_INDEX_NONE,
            VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
        goto fail;
    }

    for (size_t i = 0; i < module_count; i++) {
        const vita_binding_module *module = &modules[i];
        const vita_generated_module_descriptor *generated = module->generated;
        const vita_binding_module_metadata *post_link = module->post_link;
        if (!generated || !post_link || !generated->module_id ||
            !*generated->module_id) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_INVALID_ARGUMENT,
                VITA_BINDING_RECORD_MODULE, (uint32_t)i,
                VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
        if (generated->abi_version != VITA_GENERATED_MODULE_ABI_VERSION ||
            generated->struct_size < sizeof(*generated)) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_GENERATED_ABI,
                VITA_BINDING_RECORD_MODULE, (uint32_t)i,
                VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
        if (!loader_metadata_valid(module->loader)) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_LOADER_METADATA,
                VITA_BINDING_RECORD_MODULE, (uint32_t)i,
                VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
        for (size_t k = 0; k < i; k++) {
            const vita_loader_module_metadata *prior = modules[k].loader;
            for (uint32_t s = 0; s < module->loader->segment_count; s++) {
                const vita_loader_segment_metadata *current_segment =
                    &module->loader->segments[s];
                const uint64_t current_end =
                    (uint64_t)current_segment->virtual_address +
                    current_segment->memory_size;
                for (uint32_t p = 0; p < prior->segment_count; p++) {
                    const vita_loader_segment_metadata *prior_segment =
                        &prior->segments[p];
                    const uint64_t prior_end =
                        (uint64_t)prior_segment->virtual_address +
                        prior_segment->memory_size;
                    if ((uint64_t)current_segment->virtual_address <
                            prior_end &&
                        (uint64_t)prior_segment->virtual_address <
                            current_end) {
                        status = binding_fail(
                            error, VITA_BINDING_ERROR_LOADER_METADATA,
                            VITA_BINDING_RECORD_MODULE, (uint32_t)i,
                            s, VITA_BINDING_INDEX_NONE);
                        set_related(error, (uint32_t)k, p);
                        goto fail;
                    }
                }
            }
        }
        if (!exact_pointer_count(generated->dispatch_entries,
                                 generated->dispatch_count)) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_DISPATCH_NULL_COUNT,
                VITA_BINDING_RECORD_DISPATCH, (uint32_t)i,
                VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
        if (!exact_pointer_count(generated->variable_imports,
                                 generated->variable_import_count) ||
            !exact_pointer_count(generated->function_imports,
                                 generated->function_import_count) ||
            !exact_pointer_count(post_link->exports,
                                 post_link->export_count) ||
            !exact_pointer_count(post_link->function_imports,
                                 post_link->function_import_count) ||
            !exact_pointer_count(post_link->variable_imports,
                                 post_link->variable_import_count)) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_INVALID_ARGUMENT,
                VITA_BINDING_RECORD_MODULE, (uint32_t)i,
                VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
        if (post_link->function_import_count !=
            generated->function_import_count) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_FUNCTION_METADATA,
                VITA_BINDING_RECORD_FUNCTION_IMPORT, (uint32_t)i,
                VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
        if (post_link->variable_import_count !=
            generated->variable_import_count) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_VARIABLE_METADATA,
                VITA_BINDING_RECORD_VARIABLE_IMPORT, (uint32_t)i,
                VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
        for (size_t k = 0; k < i; k++) {
            if (strcmp(generated->module_id,
                       modules[k].generated->module_id) == 0) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_INVALID_ARGUMENT,
                    VITA_BINDING_RECORD_MODULE, (uint32_t)i,
                    VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
                set_related(error, (uint32_t)k, VITA_BINDING_INDEX_NONE);
                goto fail;
            }
        }
        if (!add_count(&dispatch_count, generated->dispatch_count) ||
            !add_count(&function_count,
                       post_link->function_import_count) ||
            !add_count(&variable_count,
                       post_link->variable_import_count)) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_NO_MEMORY,
                VITA_BINDING_RECORD_MODULE, (uint32_t)i,
                VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
        for (uint32_t v = 0; v < generated->variable_import_count; v++) {
            const vita_var_import *variable =
                &generated->variable_imports[v];
            if (!exact_pointer_count(variable->abs_sites,
                                     variable->abs_count) ||
                !add_count(&patch_count, variable->abs_count)) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_VARIABLE_METADATA,
                    VITA_BINDING_RECORD_VARIABLE_IMPORT, (uint32_t)i, v,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
        }
        for (uint32_t e = 0; e < post_link->export_count; e++) {
            if (vita_binding_export_is_global(
                    post_link->exports[e].library_nid) &&
                !add_count(&export_count, 1u)) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_NO_MEMORY,
                    VITA_BINDING_RECORD_EXPORT, (uint32_t)i, e,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
        }
    }

    if (dispatch_count) {
        dispatch = (binding_dispatch *)calloc(
            dispatch_count, sizeof(*dispatch));
        dispatch_table = (vita_dispatch_entry *)calloc(
            dispatch_count, sizeof(*dispatch_table));
    }
    if (export_count)
        result.exports = (vita_binding_export_view *)calloc(
            export_count, sizeof(*result.exports));
    if (function_count) {
        functions = (binding_function *)calloc(
            function_count, sizeof(*functions));
        result.function_imports =
            (vita_binding_function_import_view *)calloc(
                function_count, sizeof(*result.function_imports));
    }
    if (variable_count) {
        storages = (binding_storage *)calloc(
            variable_count, sizeof(*storages));
        result.variable_imports =
            (vita_binding_variable_import_view *)calloc(
                variable_count, sizeof(*result.variable_imports));
    }
    if (patch_count)
        patches = (binding_patch *)calloc(patch_count, sizeof(*patches));
    if (variable_provider_count)
        providers = (binding_provider *)calloc(
            variable_provider_count, sizeof(*providers));
    if ((dispatch_count && (!dispatch || !dispatch_table)) ||
        (export_count && !result.exports) ||
        (function_count && (!functions || !result.function_imports)) ||
        (variable_count && (!storages || !result.variable_imports)) ||
        (patch_count && !patches) ||
        (variable_provider_count && !providers)) {
        status = binding_fail(
            error, VITA_BINDING_ERROR_NO_MEMORY,
            VITA_BINDING_RECORD_NONE, VITA_BINDING_INDEX_NONE,
            VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
        goto fail;
    }

    for (size_t i = 0; i < variable_provider_count; i++) {
        providers[i].provider = variable_providers[i];
        providers[i].record_index = (uint32_t)i;
    }
    if (variable_provider_count)
        qsort(providers, variable_provider_count, sizeof(*providers),
              provider_order);
    for (size_t i = 1; i < variable_provider_count; i++) {
        if (providers[i - 1].provider.library_nid ==
                providers[i].provider.library_nid &&
            providers[i - 1].provider.variable_nid ==
                providers[i].provider.variable_nid) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_DUPLICATE_VARIABLE_PROVIDER,
                VITA_BINDING_RECORD_VARIABLE_PROVIDER,
                VITA_BINDING_INDEX_NONE, providers[i].record_index,
                VITA_BINDING_INDEX_NONE);
            set_related(error, VITA_BINDING_INDEX_NONE,
                        providers[i - 1].record_index);
            goto fail;
        }
    }

    for (size_t i = 0; i < module_count; i++) {
        const vita_binding_module *module = &modules[i];
        const vita_generated_module_descriptor *generated = module->generated;
        for (uint32_t d = 0; d < generated->dispatch_count; d++) {
            const vita_dispatch_entry *entry =
                &generated->dispatch_entries[d];
            if (!entry->fn) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_DISPATCH_NULL_FUNCTION,
                    VITA_BINDING_RECORD_DISPATCH, (uint32_t)i, d,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
            if (d && generated->dispatch_entries[d - 1].addr ==
                         entry->addr) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_DUPLICATE_DISPATCH,
                    VITA_BINDING_RECORD_DISPATCH, (uint32_t)i, d,
                    VITA_BINDING_INDEX_NONE);
                set_related(error, (uint32_t)i, d - 1u);
                goto fail;
            }
            if (d && generated->dispatch_entries[d - 1].addr >
                         entry->addr) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_DISPATCH_UNSORTED,
                    VITA_BINDING_RECORD_DISPATCH, (uint32_t)i, d,
                    VITA_BINDING_INDEX_NONE);
                set_related(error, (uint32_t)i, d - 1u);
                goto fail;
            }
            if (!loader_contains(module->loader, entry->addr & ~1u, 2u)) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_LOADER_METADATA,
                    VITA_BINDING_RECORD_DISPATCH, (uint32_t)i, d,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
            dispatch[dispatch_cursor].entry = *entry;
            dispatch[dispatch_cursor].module_index = (uint32_t)i;
            dispatch[dispatch_cursor].record_index = d;
            dispatch_cursor++;
        }

        for (uint32_t e = 0; e < module->post_link->export_count; e++) {
            const vita_binding_export_metadata *source =
                &module->post_link->exports[e];
            if (source->kind < VITA_BINDING_EXPORT_FUNCTION ||
                source->kind > VITA_BINDING_EXPORT_TLS_VARIABLE) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_INVALID_ARGUMENT,
                    VITA_BINDING_RECORD_EXPORT, (uint32_t)i, e,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
            if (!loader_contains(
                    module->loader,
                    source->kind == VITA_BINDING_EXPORT_FUNCTION
                        ? source->address & ~1u : source->address,
                    source->kind == VITA_BINDING_EXPORT_FUNCTION ? 2u : 1u)) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_EXPORT_RANGE,
                    VITA_BINDING_RECORD_EXPORT, (uint32_t)i, e,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
            if (!vita_binding_export_is_global(source->library_nid))
                continue;
            result.exports[export_cursor] =
                (vita_binding_export_view){
                    source->kind, source->library_nid, source->symbol_nid,
                    source->address, (uint32_t)i, e
                };
            export_cursor++;
        }
    }

    if (dispatch_count)
        qsort(dispatch, dispatch_count, sizeof(*dispatch), dispatch_order);
    for (size_t i = 1; i < dispatch_count; i++) {
        if (dispatch[i - 1].entry.addr == dispatch[i].entry.addr) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_DUPLICATE_DISPATCH,
                VITA_BINDING_RECORD_DISPATCH, dispatch[i].module_index,
                dispatch[i].record_index, VITA_BINDING_INDEX_NONE);
            set_related(error, dispatch[i - 1].module_index,
                        dispatch[i - 1].record_index);
            goto fail;
        }
    }

    if (export_count)
        qsort(result.exports, export_count, sizeof(*result.exports),
              export_order);
    for (size_t i = 1; i < export_count; i++) {
        const vita_binding_export_view *prior = &result.exports[i - 1];
        const vita_binding_export_view *current = &result.exports[i];
        if (prior->library_nid == current->library_nid &&
            prior->symbol_nid == current->symbol_nid) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_DUPLICATE_EXPORT,
                VITA_BINDING_RECORD_EXPORT, current->module_index,
                current->record_index, VITA_BINDING_INDEX_NONE);
            set_related(error, prior->module_index, prior->record_index);
            goto fail;
        }
    }
    for (size_t i = 0; i < export_count; i++) {
        const vita_binding_export_view *item = &result.exports[i];
        if (item->kind != VITA_BINDING_EXPORT_FUNCTION) continue;
        if (find_dispatch(dispatch, dispatch_count, item->address)) continue;
        status = binding_fail(
            error,
            find_aligned_dispatch(dispatch, dispatch_count, item->address)
                ? VITA_BINDING_ERROR_MODE_MISMATCH
                : VITA_BINDING_ERROR_EXPORT_DISPATCH,
            VITA_BINDING_RECORD_EXPORT, item->module_index,
            item->record_index, VITA_BINDING_INDEX_NONE);
        goto fail;
    }

    for (size_t i = 0; i < module_count; i++) {
        const vita_binding_module *module = &modules[i];
        for (uint32_t f = 0;
             f < module->post_link->function_import_count; f++) {
            const vita_binding_function_import_metadata *metadata =
                &module->post_link->function_imports[f];
            const vita_fn_import *generated_import = NULL;
            uint32_t metadata_matches = 0;
            binding_dispatch *stub =
                NULL;
            vita_binding_export_view *export_item;
            vita_hle_fn hle = NULL;
            for (uint32_t g = 0;
                 g < module->generated->function_import_count; g++) {
                const vita_fn_import *candidate =
                    &module->generated->function_imports[g];
                if (!same_function_record(candidate, metadata)) continue;
                generated_import = candidate;
                metadata_matches++;
            }
            if (metadata_matches != 1u || !generated_import) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_FUNCTION_METADATA,
                    VITA_BINDING_RECORD_FUNCTION_IMPORT, (uint32_t)i, f,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
            for (size_t prior = 0; prior < function_cursor; prior++) {
                if (functions[prior].generated == generated_import ||
                    functions[prior].generated->target_slot ==
                        generated_import->target_slot) {
                    status = binding_fail(
                        error, VITA_BINDING_ERROR_FUNCTION_METADATA,
                        VITA_BINDING_RECORD_FUNCTION_IMPORT, (uint32_t)i, f,
                        VITA_BINDING_INDEX_NONE);
                    set_related(error, functions[prior].module_index,
                                functions[prior].record_index);
                    goto fail;
                }
            }
            stub = find_dispatch(dispatch, dispatch_count,
                                 generated_import->dispatch_key);
            if (!stub || stub->module_index != i) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_FUNCTION_IMPORT_DISPATCH,
                    VITA_BINDING_RECORD_FUNCTION_IMPORT, (uint32_t)i, f,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
            if (stub->import_claimed) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_FUNCTION_IMPORT_DISPATCH,
                    VITA_BINDING_RECORD_FUNCTION_IMPORT, (uint32_t)i, f,
                    VITA_BINDING_INDEX_NONE);
                set_related(error, stub->module_index, stub->record_index);
                goto fail;
            }
            stub->import_claimed = 1;
            if (!generated_import->target_slot ||
                !*generated_import->target_slot ||
                generated_import->strict_unsupported > 1u ||
                (generated_import->strict_unsupported &&
                 (!generated_import->name || !*generated_import->name))) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_FUNCTION_METADATA,
                    VITA_BINDING_RECORD_FUNCTION_IMPORT, (uint32_t)i, f,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
            if ((metadata->target_site & 3u) ||
                !loader_contains(module->loader,
                                 metadata->target_site, 4u)) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_FUNCTION_TARGET_SITE,
                    VITA_BINDING_RECORD_FUNCTION_IMPORT, (uint32_t)i, f,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }

            functions[function_cursor].metadata = metadata;
            functions[function_cursor].generated = generated_import;
            functions[function_cursor].module_index = (uint32_t)i;
            functions[function_cursor].record_index = f;
            functions[function_cursor].previous =
                *generated_import->target_slot;
            export_item = find_export(
                result.exports, export_count,
                metadata->library_nid, metadata->function_nid);
            if (export_item) {
                binding_dispatch *target;
                if (export_item->kind != VITA_BINDING_EXPORT_FUNCTION) {
                    status = binding_fail(
                        error, VITA_BINDING_ERROR_UNRESOLVED_FUNCTION,
                        VITA_BINDING_RECORD_FUNCTION_IMPORT, (uint32_t)i, f,
                        VITA_BINDING_INDEX_NONE);
                    set_related(error, export_item->module_index,
                                export_item->record_index);
                    goto fail;
                }
                if (export_item->address != metadata->target_address) {
                    status = binding_fail(
                        error,
                        (export_item->address & ~1u) ==
                                (metadata->target_address & ~1u)
                            ? VITA_BINDING_ERROR_MODE_MISMATCH
                            : VITA_BINDING_ERROR_FUNCTION_TARGET_MISMATCH,
                        VITA_BINDING_RECORD_FUNCTION_IMPORT,
                        (uint32_t)i, f, VITA_BINDING_INDEX_NONE);
                    set_related(error, export_item->module_index,
                                export_item->record_index);
                    goto fail;
                }
                target = find_dispatch(
                    dispatch, dispatch_count, metadata->target_address);
                if (!target) {
                    status = binding_fail(
                        error,
                        find_aligned_dispatch(
                            dispatch, dispatch_count,
                            metadata->target_address)
                            ? VITA_BINDING_ERROR_MODE_MISMATCH
                            : VITA_BINDING_ERROR_FUNCTION_TARGET_MISMATCH,
                        VITA_BINDING_RECORD_FUNCTION_IMPORT,
                        (uint32_t)i, f, VITA_BINDING_INDEX_NONE);
                    goto fail;
                }
                functions[function_cursor].function = target->entry.fn;
                functions[function_cursor].resolution =
                    VITA_BINDING_FUNCTION_GENERATED;
            } else {
                int hle_result;
                if (metadata->library_nid && metadata->function_nid)
                    hle_result = vita_hle_resolve_registered(
                        metadata->library_nid, metadata->function_nid,
                        NULL, NULL, &hle);
                else
                    hle_result = vita_hle_resolve_registered(
                        0, 0, NULL, NULL, &hle);
                if (hle_result == VITA_OK) {
                    functions[function_cursor].function = hle;
                    functions[function_cursor].resolution =
                        VITA_BINDING_FUNCTION_HLE;
                } else if (generated_import->strict_unsupported &&
                           generated_import->name &&
                           *generated_import->name &&
                           module->generated->report.strict_unsupported) {
                    functions[function_cursor].function = stub->entry.fn;
                    functions[function_cursor].resolution =
                        VITA_BINDING_FUNCTION_STRICT_UNSUPPORTED;
                } else {
                    status = binding_fail(
                        error, VITA_BINDING_ERROR_UNRESOLVED_FUNCTION,
                        VITA_BINDING_RECORD_FUNCTION_IMPORT,
                        (uint32_t)i, f, VITA_BINDING_INDEX_NONE);
                    goto fail;
                }
            }
            stub->entry.fn = functions[function_cursor].function;
            result.function_imports[function_cursor] =
                (vita_binding_function_import_view){
                    metadata->library_nid, metadata->function_nid,
                    generated_import->dispatch_key,
                    metadata->target_address,
                    (uint32_t)i, f, functions[function_cursor].resolution
                };
            function_cursor++;
        }
    }

    for (size_t i = 0; i < module_count; i++) {
        const vita_binding_module *module = &modules[i];
        const vita_generated_module_descriptor *generated = module->generated;
        const vita_binding_module_metadata *post_link = module->post_link;
        for (uint32_t g = 0; g < generated->variable_import_count; g++) {
            const vita_var_import *variable =
                &generated->variable_imports[g];
            const vita_binding_variable_import_metadata *metadata = NULL;
            uint32_t metadata_index = VITA_BINDING_INDEX_NONE;
            uint32_t matches = 0;
            uint32_t address = 0;
            vita_binding_export_view *export_item;
            binding_provider *provider;

            for (uint32_t v = 0; v < post_link->variable_import_count; v++) {
                if (!same_variable_key(variable,
                                       &post_link->variable_imports[v]))
                    continue;
                metadata = &post_link->variable_imports[v];
                metadata_index = v;
                matches++;
            }
            if (matches != 1u || !variable->value || !metadata ||
                !exact_pointer_count(metadata->abs_sites,
                                     metadata->abs_count) ||
                metadata->abs_count != variable->abs_count ||
                (!metadata->target_address && metadata->abs_count)) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_VARIABLE_METADATA,
                    VITA_BINDING_RECORD_VARIABLE_IMPORT, (uint32_t)i, g,
                    VITA_BINDING_INDEX_NONE);
                goto fail;
            }
            for (uint32_t prior = 0; prior < g; prior++) {
                if (generated->variable_imports[prior].lib_nid ==
                        variable->lib_nid &&
                    generated->variable_imports[prior].var_nid ==
                        variable->var_nid) {
                    status = binding_fail(
                        error, VITA_BINDING_ERROR_VARIABLE_METADATA,
                        VITA_BINDING_RECORD_VARIABLE_IMPORT,
                        (uint32_t)i, g, VITA_BINDING_INDEX_NONE);
                    set_related(error, (uint32_t)i, prior);
                    goto fail;
                }
            }

            export_item = find_export(
                result.exports, export_count,
                variable->lib_nid, variable->var_nid);
            provider = find_provider(
                providers, variable_provider_count,
                variable->lib_nid, variable->var_nid);
            if (export_item && provider) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_AMBIGUOUS_VARIABLE,
                    VITA_BINDING_RECORD_VARIABLE_IMPORT, (uint32_t)i,
                    metadata_index, VITA_BINDING_INDEX_NONE);
                set_related(error, export_item->module_index,
                            export_item->record_index);
                goto fail;
            }
            if (export_item) {
                if (export_item->kind == VITA_BINDING_EXPORT_FUNCTION) {
                    status = binding_fail(
                        error, VITA_BINDING_ERROR_UNRESOLVED_VARIABLE,
                        VITA_BINDING_RECORD_VARIABLE_IMPORT, (uint32_t)i,
                        metadata_index, VITA_BINDING_INDEX_NONE);
                    set_related(error, export_item->module_index,
                                export_item->record_index);
                    goto fail;
                }
                address = export_item->address;
            } else if (provider) {
                address = provider->provider.address;
            } else {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_UNRESOLVED_VARIABLE,
                    VITA_BINDING_RECORD_VARIABLE_IMPORT, (uint32_t)i,
                    metadata_index, VITA_BINDING_INDEX_NONE);
                goto fail;
            }
            if (metadata->target_address &&
                metadata->target_address != address) {
                status = binding_fail(
                    error, VITA_BINDING_ERROR_VARIABLE_TARGET_MISMATCH,
                    VITA_BINDING_RECORD_VARIABLE_IMPORT, (uint32_t)i,
                    metadata_index, VITA_BINDING_INDEX_NONE);
                goto fail;
            }

            for (uint32_t s = 0; s < variable->abs_count; s++) {
                const vita_var_site *site = &variable->abs_sites[s];
                uint32_t metadata_site = VITA_BINDING_INDEX_NONE;
                if (!find_metadata_site(metadata, site, &metadata_site)) {
                    status = binding_fail(
                        error, VITA_BINDING_ERROR_VARIABLE_METADATA,
                        VITA_BINDING_RECORD_VARIABLE_SITE, (uint32_t)i,
                        metadata_index, s);
                    goto fail;
                }
                for (uint32_t prior = 0; prior < s; prior++) {
                    if (variable->abs_sites[prior].addr == site->addr) {
                        status = binding_fail(
                            error, VITA_BINDING_ERROR_VARIABLE_METADATA,
                            VITA_BINDING_RECORD_VARIABLE_SITE,
                            (uint32_t)i, metadata_index, s);
                        goto fail;
                    }
                }
                if ((site->addr & 3u) ||
                    !loader_contains(module->loader, site->addr, 4u)) {
                    status = binding_fail(
                        error, VITA_BINDING_ERROR_PATCH_SITE,
                        VITA_BINDING_RECORD_VARIABLE_SITE, (uint32_t)i,
                        metadata_index, metadata_site);
                    goto fail;
                }
                if (site->addend > UINT32_MAX - address) {
                    status = binding_fail(
                        error, VITA_BINDING_ERROR_PATCH_ADDEND_OVERFLOW,
                        VITA_BINDING_RECORD_VARIABLE_SITE, (uint32_t)i,
                        metadata_index, metadata_site);
                    goto fail;
                }
                patches[patch_cursor++] = (binding_patch){
                    site->addr, address + site->addend, 0u, (uint32_t)i,
                    metadata_index, metadata_site
                };
            }

            for (size_t prior = 0; prior < variable_cursor; prior++) {
                if (storages[prior].pointer == variable->value) {
                    status = binding_fail(
                        error, VITA_BINDING_ERROR_VARIABLE_METADATA,
                        VITA_BINDING_RECORD_VARIABLE_IMPORT, (uint32_t)i,
                        metadata_index, VITA_BINDING_INDEX_NONE);
                    set_related(error, storages[prior].module_index,
                                storages[prior].record_index);
                    goto fail;
                }
            }
            storages[variable_cursor] = (binding_storage){
                variable->value, address, 0u, (uint32_t)i, metadata_index
            };
            result.variable_imports[variable_cursor] =
                (vita_binding_variable_import_view){
                    variable->lib_nid, variable->var_nid, address,
                    (uint32_t)i, metadata_index
                };
            variable_cursor++;
        }
    }

    if (patch_count)
        qsort(patches, patch_count, sizeof(*patches), patch_order);
    for (size_t i = 1; i < patch_count; i++) {
        if (patches[i - 1].address == patches[i].address) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_PATCH_SITE,
                VITA_BINDING_RECORD_VARIABLE_SITE, patches[i].module_index,
                patches[i].record_index, patches[i].site_index);
            set_related(error, patches[i - 1].module_index,
                        patches[i - 1].record_index);
            goto fail;
        }
    }

    if (!vita_runtime_can_bind()) {
        status = binding_fail(
            error, VITA_BINDING_ERROR_NOT_INITIALIZED,
            VITA_BINDING_RECORD_NONE, VITA_BINDING_INDEX_NONE,
            VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
        goto fail;
    }

    vita_memory_lock();
    memory_locked = 1;
    for (size_t i = 0; i < dispatch_count; i++) {
        if (!vita_mem_ptr(dispatch[i].entry.addr & ~1u, 2u)) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_LOADER_METADATA,
                VITA_BINDING_RECORD_DISPATCH, dispatch[i].module_index,
                dispatch[i].record_index, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
    }
    for (size_t i = 0; i < export_count; i++) {
        const vita_binding_export_view *item = &result.exports[i];
        if (!vita_mem_ptr(
                item->kind == VITA_BINDING_EXPORT_FUNCTION
                    ? item->address & ~1u : item->address,
                item->kind == VITA_BINDING_EXPORT_FUNCTION ? 2u : 1u)) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_EXPORT_RANGE,
                VITA_BINDING_RECORD_EXPORT, item->module_index,
                item->record_index, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
    }
    for (size_t i = 0; i < variable_provider_count; i++) {
        if (!vita_mem_ptr(providers[i].provider.address, 1u)) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_EXPORT_RANGE,
                VITA_BINDING_RECORD_VARIABLE_PROVIDER,
                VITA_BINDING_INDEX_NONE, providers[i].record_index,
                VITA_BINDING_INDEX_NONE);
            goto fail;
        }
    }
    for (size_t i = 0; i < function_count; i++) {
        void *pointer = vita_mem_ptr(
            functions[i].metadata->target_site, sizeof(uint32_t));
        uint32_t loaded = 0;
        if (!pointer) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_FUNCTION_TARGET_SITE,
                VITA_BINDING_RECORD_FUNCTION_IMPORT,
                functions[i].module_index, functions[i].record_index,
                VITA_BINDING_INDEX_NONE);
            goto fail;
        }
        memcpy(&loaded, pointer, sizeof(loaded));
        if (loaded != functions[i].metadata->target_address) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_FUNCTION_TARGET_MISMATCH,
                VITA_BINDING_RECORD_FUNCTION_IMPORT,
                functions[i].module_index, functions[i].record_index,
                VITA_BINDING_INDEX_NONE);
            goto fail;
        }
    }
    for (size_t i = 0; i < patch_count; i++) {
        void *pointer = vita_mem_ptr(patches[i].address, sizeof(uint32_t));
        uint32_t loaded = 0;
        if (!pointer) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_PATCH_SITE,
                VITA_BINDING_RECORD_VARIABLE_SITE, patches[i].module_index,
                patches[i].record_index, patches[i].site_index);
            goto fail;
        }
        memcpy(&loaded, pointer, sizeof(loaded));
        if (loaded != patches[i].value) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_PATCH_VALUE_MISMATCH,
                VITA_BINDING_RECORD_VARIABLE_SITE, patches[i].module_index,
                patches[i].record_index, patches[i].site_index);
            goto fail;
        }
        patches[i].previous = loaded;
    }

    for (size_t i = 0; i < dispatch_count; i++)
        dispatch_table[i] = dispatch[i].entry;
    for (size_t i = 0; i < variable_count; i++)
        storages[i].previous = *storages[i].pointer;
    for (size_t i = 0; i < function_count; i++)
        *functions[i].generated->target_slot = functions[i].function;
    for (size_t i = 0; i < variable_count; i++)
        *storages[i].pointer = storages[i].value;
    for (size_t i = 0; i < patch_count; i++) {
        void *pointer = vita_mem_ptr(patches[i].address, sizeof(uint32_t));
        memcpy(pointer, &patches[i].value, sizeof(patches[i].value));
    }
    if (patch_count) vita_exclusive_note_write();
    mutations_applied = 1;
    {
        vita_dispatch_status dispatch_status = vita_dispatch_init_checked(
            dispatch_count ? dispatch_table : NULL,
            (uint32_t)dispatch_count);
        if (dispatch_status != VITA_DISPATCH_OK) {
            status = binding_fail(
                error, VITA_BINDING_ERROR_DISPATCH_INSTALL,
                VITA_BINDING_RECORD_DISPATCH, VITA_BINDING_INDEX_NONE,
                VITA_BINDING_INDEX_NONE, VITA_BINDING_INDEX_NONE);
            goto fail;
        }
    }

    mutations_applied = 0;
    vita_memory_unlock();
    memory_locked = 0;

    result.export_count = (uint32_t)export_count;
    result.function_import_count = (uint32_t)function_count;
    result.variable_import_count = (uint32_t)variable_count;
    result.dispatch_count = (uint32_t)dispatch_count;
    if (function_count)
        qsort(result.function_imports, function_count,
              sizeof(*result.function_imports), function_view_order);
    if (variable_count)
        qsort(result.variable_imports, variable_count,
              sizeof(*result.variable_imports), variable_view_order);
    *view = result;
    memset(&result, 0, sizeof(result));
    clear_error(error);

fail:
    if (mutations_applied) {
        for (size_t i = function_count; i > 0; i--)
            *functions[i - 1].generated->target_slot =
                functions[i - 1].previous;
        for (size_t i = variable_count; i > 0; i--)
            *storages[i - 1].pointer = storages[i - 1].previous;
        for (size_t i = patch_count; i > 0; i--) {
            void *pointer = vita_mem_ptr(
                patches[i - 1].address, sizeof(uint32_t));
            if (pointer)
                memcpy(pointer, &patches[i - 1].previous,
                       sizeof(patches[i - 1].previous));
        }
        if (patch_count) vita_exclusive_note_write();
    }
    if (memory_locked) vita_memory_unlock();
    free(dispatch);
    free(dispatch_table);
    free(providers);
    free(functions);
    free(storages);
    free(patches);
    vita_binding_view_destroy(&result);
    mtx_unlock(&g_binding_lock);
    return status;
}

const char *vita_binding_status_string(vita_binding_status status) {
    switch (status) {
    case VITA_BINDING_OK: return "ok";
    case VITA_BINDING_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case VITA_BINDING_ERROR_NOT_INITIALIZED:
        return "runtime is not ready for binding";
    case VITA_BINDING_ERROR_NO_MEMORY: return "out of memory";
    case VITA_BINDING_ERROR_GENERATED_ABI:
        return "generated-module ABI mismatch";
    case VITA_BINDING_ERROR_LOADER_METADATA:
        return "invalid strict-loader metadata";
    case VITA_BINDING_ERROR_DISPATCH_NULL_COUNT:
        return "dispatch table/count mismatch";
    case VITA_BINDING_ERROR_DISPATCH_NULL_FUNCTION:
        return "null dispatch function";
    case VITA_BINDING_ERROR_DISPATCH_UNSORTED:
        return "module dispatch keys are unsorted";
    case VITA_BINDING_ERROR_DUPLICATE_DISPATCH:
        return "duplicate dispatch key";
    case VITA_BINDING_ERROR_DUPLICATE_EXPORT:
        return "duplicate export";
    case VITA_BINDING_ERROR_EXPORT_RANGE:
        return "export/provider address is outside loaded memory";
    case VITA_BINDING_ERROR_EXPORT_DISPATCH:
        return "function export has no dispatch key";
    case VITA_BINDING_ERROR_MODE_MISMATCH:
        return "ARM/Thumb dispatch mode mismatch";
    case VITA_BINDING_ERROR_FUNCTION_IMPORT_DISPATCH:
        return "function import has no unique dispatch stub";
    case VITA_BINDING_ERROR_FUNCTION_TARGET_SITE:
        return "invalid function target site";
    case VITA_BINDING_ERROR_FUNCTION_TARGET_MISMATCH:
        return "loaded function target disagrees with metadata";
    case VITA_BINDING_ERROR_UNRESOLVED_FUNCTION:
        return "unresolved function import";
    case VITA_BINDING_ERROR_DUPLICATE_VARIABLE_PROVIDER:
        return "duplicate variable provider";
    case VITA_BINDING_ERROR_VARIABLE_METADATA:
        return "generated variable metadata mismatch";
    case VITA_BINDING_ERROR_AMBIGUOUS_VARIABLE:
        return "ambiguous variable source";
    case VITA_BINDING_ERROR_UNRESOLVED_VARIABLE:
        return "unresolved variable import";
    case VITA_BINDING_ERROR_VARIABLE_TARGET_MISMATCH:
        return "loaded variable target disagrees with metadata";
    case VITA_BINDING_ERROR_PATCH_SITE:
        return "invalid or duplicate ABS32 patch site";
    case VITA_BINDING_ERROR_PATCH_ADDEND_OVERFLOW:
        return "ABS32 patch addend overflows";
    case VITA_BINDING_ERROR_PATCH_VALUE_MISMATCH:
        return "loaded ABS32 word disagrees with binding metadata";
    case VITA_BINDING_ERROR_DISPATCH_INSTALL:
        return "merged dispatch installation failed";
    case VITA_BINDING_ERROR_FUNCTION_METADATA:
        return "generated function-import metadata mismatch";
    default:
        return "unknown binding status";
    }
}
