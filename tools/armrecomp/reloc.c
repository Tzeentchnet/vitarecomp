/* Tool-facing adapter for the framework's shared relocation parser. */

#include "reloc.h"

#include <stdio.h>
#include <string.h>

static void module_view(
    const vc_module *source,
    vita_reloc_program_header *headers,
    vita_reloc_module *module) {
    int index;
    memset(headers, 0, VC_MAX_SEGMENTS * sizeof(*headers));
    for (index = 0; index < source->phdr_count; index++) {
        headers[index].type = source->phdr[index].type;
        headers[index].file_offset = source->phdr[index].offset;
        headers[index].virtual_address = source->phdr[index].vaddr;
        headers[index].file_size = source->phdr[index].filesz;
        headers[index].memory_size = source->phdr[index].memsz;
    }
    module->program_headers = headers;
    module->program_header_count = (uint32_t)source->phdr_count;
}

static void copy_error(
    char *destination,
    size_t destination_size,
    const vita_reloc_error *error) {
    if (!destination_size) return;
    (void)snprintf(destination, destination_size, "%s",
                   error && error->message[0]
                       ? error->message
                       : "relocation processing failed");
}

int vr_parse(const uint8_t *buf, size_t size, const vc_module *source,
             vr_table *out, char *err, size_t errsz) {
    vita_reloc_program_header headers[VC_MAX_SEGMENTS];
    vita_reloc_module module;
    vita_reloc_error error;
    vita_reloc_status status;
    if (!source || source->phdr_count < 0 ||
        source->phdr_count > VC_MAX_SEGMENTS) {
        if (out) memset(out, 0, sizeof(*out));
        if (errsz) (void)snprintf(err, errsz, "invalid relocation module");
        return 1;
    }
    module_view(source, headers, &module);
    status = vita_reloc_parse(buf, size, &module, out, &error);
    if (status != VITA_RELOC_OK) copy_error(err, errsz, &error);
    else if (errsz) err[0] = '\0';
    return status != VITA_RELOC_OK;
}

int vr_apply(uint8_t *buf, size_t size, const vc_module *source,
             const vr_table *table, uint32_t *changed,
             uint32_t *unsupported, uint32_t *outside) {
    vita_reloc_program_header headers[VC_MAX_SEGMENTS];
    vita_reloc_module module;
    vita_reloc_identity result;
    vita_reloc_status status;
    if (!source || !changed || !unsupported || !outside ||
        source->phdr_count < 0 || source->phdr_count > VC_MAX_SEGMENTS)
        return 1;
    module_view(source, headers, &module);
    status = vita_reloc_apply(buf, size, &module, table, &result);
    if (status != VITA_RELOC_OK) return 1;
    *changed = result.changed;
    *unsupported = result.unsupported;
    *outside = result.outside;
    return 0;
}

void vr_free(vr_table *table) {
    vita_reloc_free(table);
}

const char *vr_code_name(uint8_t code) {
    return vita_reloc_code_name(code);
}
