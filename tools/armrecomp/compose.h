/* compose.h -- deterministic composition of generated module artifacts. */

#ifndef ARMRECOMP_COMPOSE_H
#define ARMRECOMP_COMPOSE_H

#include <stddef.h>

/*
 * `inputs` is an unordered collection containing one schema-v4 .module.json
 * and one schema-v4 .report.json for every module.  Composition writes
 * `out_c`, the matching header, and matching .report.json/.report.txt files.
 * Input paths are diagnostics only and are never copied into an artifact.
 */
int ac_compose(const char *out_c, const char *entry_module_id,
               const char *const *inputs, size_t input_count,
               char *error, size_t error_size);

#endif /* ARMRECOMP_COMPOSE_H */
