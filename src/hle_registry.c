/* SPDX-License-Identifier: MIT */
#include "vitarecomp/hle.h"

#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/runtime.h"
#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define SCE_KERNEL_ERROR_UNIMPLEMENTED UINT32_C(0x80020001)
#define HLE_IMPORT_LIMIT 4096u

typedef struct registered_import {
    uint32_t library_nid;
    uint32_t function_nid;
    char *library_name;
    char *function_name;
    vita_hle_fn function;
} registered_import;

static once_flag g_once = ONCE_FLAG_INIT;
static mtx_t g_lock;
static registered_import *g_imports;
static size_t g_count;
static size_t g_capacity;

static void registry_once(void) {
    (void)mtx_init(&g_lock, mtx_plain);
}

static char *copy_string(const char *value) {
    if (!value) return NULL;
    size_t length = strlen(value);
    if (length > 255) return NULL;
    char *copy = (char *)malloc(length + 1);
    if (copy) memcpy(copy, value, length + 1);
    return copy;
}

static int same_import(const registered_import *left,
                       const vita_hle_import *right) {
    if (left->function_nid && right->function_nid &&
        left->function_nid == right->function_nid &&
        (!left->library_nid || !right->library_nid ||
         left->library_nid == right->library_nid))
        return 1;
    return left->function_name && right->function_name &&
           strcmp(left->function_name, right->function_name) == 0 &&
           ((!left->library_name && !right->library_name) ||
            (left->library_name && right->library_name &&
             strcmp(left->library_name, right->library_name) == 0));
}

int vita_hle_register(const vita_hle_import *imports, size_t count) {
    if ((!imports && count) || count > HLE_IMPORT_LIMIT)
        return VITA_ERROR_INVALID_ARGUMENT;
    call_once(&g_once, registry_once);
    mtx_lock(&g_lock);
    if (count > HLE_IMPORT_LIMIT - g_count) {
        mtx_unlock(&g_lock);
        return VITA_ERROR_LIMIT;
    }
    for (size_t i = 0; i < count; i++) {
        const vita_hle_import *candidate = &imports[i];
        if (!candidate->function ||
            (!candidate->function_nid && !candidate->function_name) ||
            (candidate->library_name &&
             strlen(candidate->library_name) > 255) ||
            (candidate->function_name &&
             strlen(candidate->function_name) > 255)) {
            mtx_unlock(&g_lock);
            return VITA_ERROR_INVALID_ARGUMENT;
        }
        for (size_t k = 0; k < g_count; k++) {
            if (same_import(&g_imports[k], candidate)) {
                mtx_unlock(&g_lock);
                return VITA_ERROR_ALREADY_INITIALIZED;
            }
        }
        for (size_t k = 0; k < i; k++) {
            registered_import prior;
            memset(&prior, 0, sizeof(prior));
            prior.library_nid = imports[k].library_nid;
            prior.function_nid = imports[k].function_nid;
            prior.library_name = (char *)imports[k].library_name;
            prior.function_name = (char *)imports[k].function_name;
            if (same_import(&prior, candidate)) {
                mtx_unlock(&g_lock);
                return VITA_ERROR_ALREADY_INITIALIZED;
            }
        }
    }

    if (g_count + count > g_capacity) {
        size_t capacity = g_capacity ? g_capacity : 64;
        while (capacity < g_count + count) capacity *= 2;
        registered_import *grown = (registered_import *)realloc(
            g_imports, capacity * sizeof(*grown));
        if (!grown) {
            mtx_unlock(&g_lock);
            return VITA_ERROR_NO_MEMORY;
        }
        g_imports = grown;
        g_capacity = capacity;
    }

    size_t added = 0;
    for (; added < count; added++) {
        const vita_hle_import *source = &imports[added];
        registered_import entry;
        memset(&entry, 0, sizeof(entry));
        entry.library_nid = source->library_nid;
        entry.function_nid = source->function_nid;
        entry.function = source->function;
        entry.library_name = copy_string(source->library_name);
        entry.function_name = copy_string(source->function_name);
        if ((source->library_name && !entry.library_name) ||
            (source->function_name && !entry.function_name)) {
            free(entry.library_name);
            free(entry.function_name);
            break;
        }
        g_imports[g_count++] = entry;
    }
    if (added != count) {
        while (added > 0) {
            added--;
            registered_import *entry = &g_imports[--g_count];
            free(entry->library_name);
            free(entry->function_name);
        }
        mtx_unlock(&g_lock);
        return VITA_ERROR_NO_MEMORY;
    }
    mtx_unlock(&g_lock);
    return VITA_OK;
}

void vita_hle_registry_reset(void) {
    call_once(&g_once, registry_once);
    mtx_lock(&g_lock);
    for (size_t i = 0; i < g_count; i++) {
        free(g_imports[i].library_name);
        free(g_imports[i].function_name);
    }
    free(g_imports);
    g_imports = NULL;
    g_count = 0;
    g_capacity = 0;
    mtx_unlock(&g_lock);
}

int vita_hle_resolve_registered(uint32_t library_nid, uint32_t function_nid,
                                const char *library_name,
                                const char *function_name,
                                vita_hle_fn *function) {
    if (!function) return VITA_ERROR_INVALID_ARGUMENT;
    *function = NULL;
    const int by_nid = library_nid != 0u && function_nid != 0u;
    const int by_name = library_nid == 0u && function_nid == 0u &&
                        library_name && *library_name &&
                        function_name && *function_name;
    if (!by_nid && !by_name) return VITA_ERROR_INVALID_ARGUMENT;

    call_once(&g_once, registry_once);
    mtx_lock(&g_lock);
    for (size_t i = 0; i < g_count; i++) {
        const registered_import *entry = &g_imports[i];
        int match = by_nid
            ? entry->library_nid == library_nid &&
              entry->function_nid == function_nid
            : entry->library_name && entry->function_name &&
              strcmp(entry->library_name, library_name) == 0 &&
              strcmp(entry->function_name, function_name) == 0;
        if (!match) continue;
        *function = entry->function;
        break;
    }
    mtx_unlock(&g_lock);
    return *function ? VITA_OK : VITA_ERROR_NOT_FOUND;
}

int vita_hle_unimplemented(uint32_t stub_address, uint32_t library_nid,
                           uint32_t function_nid,
                           const char *library_name,
                           const char *function_name) {
    r0 = SCE_KERNEL_ERROR_UNIMPLEMENTED;
    vita_runtime_count(VITA_COUNTER_IMPORT_UNIMPLEMENTED, 1);
    vita_diagnostic event = {
        VITA_DIAG_ERROR,
        VITA_DIAG_IMPORT,
        VITA_ERROR_UNIMPLEMENTED,
        stub_address,
        library_nid,
        function_nid,
        library_name,
        function_name ? function_name : "(unresolved NID)",
        "unimplemented firmware import"
    };
    vita_runtime_emit(&event);
    return VITA_ERROR_UNIMPLEMENTED;
}

int vita_hle_dispatch_import(uint32_t stub_address, uint32_t library_nid,
                             uint32_t function_nid,
                             const char *library_name,
                             const char *function_name) {
    vita_hle_fn function = NULL;
    char resolved_library_storage[256];
    char resolved_function_storage[256];
    const char *resolved_library = library_name;
    const char *resolved_function = function_name;
    call_once(&g_once, registry_once);
    mtx_lock(&g_lock);
    for (size_t i = 0; i < g_count; i++) {
        registered_import *entry = &g_imports[i];
        int nid_match = function_nid && entry->function_nid == function_nid &&
                        (!library_nid || !entry->library_nid ||
                         library_nid == entry->library_nid);
        int name_match = function_name && entry->function_name &&
                         strcmp(function_name, entry->function_name) == 0 &&
                         (!library_name || !entry->library_name ||
                          strcmp(library_name, entry->library_name) == 0);
        if (!nid_match && !name_match) continue;
        function = entry->function;
        if (entry->library_name) {
            memcpy(resolved_library_storage, entry->library_name,
                   strlen(entry->library_name) + 1);
            resolved_library = resolved_library_storage;
        }
        if (entry->function_name) {
            memcpy(resolved_function_storage, entry->function_name,
                   strlen(entry->function_name) + 1);
            resolved_function = resolved_function_storage;
        }
        break;
    }
    mtx_unlock(&g_lock);

    if (!function)
        return vita_hle_unimplemented(stub_address, library_nid, function_nid,
                                      library_name, function_name);

    vita_diagnostic event = {
        VITA_DIAG_TRACE,
        VITA_DIAG_IMPORT,
        VITA_OK,
        stub_address,
        library_nid,
        function_nid,
        resolved_library,
        resolved_function,
        "dispatching firmware import"
    };
    vita_runtime_emit(&event);
    vita_runtime_count(VITA_COUNTER_IMPORT_DISPATCHED, 1);
    function();
    return VITA_OK;
}
